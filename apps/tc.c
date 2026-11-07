#include "aptos.h"

/* tc.app — TinyC 编译器（aptos 用户态，第一版；完全自研，不借用现成编译器实现）
 *
 * 语言：
 *   int f(int a, int b) { ... }        函数，最多 6 个参数
 *   int x;  int x = expr;              局部变量
 *   int a[N];                          局部数组（栈上，N 为常量）
 *   x = expr;  a[i] = expr;            赋值
 *   if (c) stmt else stmt              条件
 *   while (c) stmt                     循环
 *   return expr;                       返回（= 进程退出码）
 *   print("text");                     输出字符串
 *   表达式：+ - * / % < > <= >= == != ! && || ( ) 数字 变量 a[i] 调用
 *   内置：sys4(n,a,b,c) → int 0x80，返回 eax
 *
 * 产物：ELF32（vaddr 0xC0000000，单 PT_LOAD，入口 = 映像内 stub），aptos elf_load 直接可跑。
 * 自举缺口：无全局变量、无指针（除数组下标）、无结构体、无字符串变量、无预处理、单文件、无浮点。
 */

#define CODE_BASE 0xC0000000
#define CODE_MAX  60000
#define SRC_MAX   49152
#define STR_MAX   8192
#define MAX_FUNCS 64
#define MAX_VARS  128
#define MAX_PATCH 512
#define MAX_STR   128
#define HDR_SIZE  84
#define GC_BASE   0x10000       /* 全局数据区相对 CODE_BASE 的偏移（BSS，清零） */
#define GDATA_MAX 65536
#define MAX_GVARS 64
#define MAX_STRUCTS 16
#define MAX_FIELDS  12

static unsigned char code[CODE_MAX];
static int code_len;
static int cur_line = 1;

static int expr_unsigned;      /* 当前表达式是否无符号 */
static int last_type_unsigned; /* skip_type_prefix 的副产物 */
static int last_type_struct = -1;  /* >=0 表示该类型是结构体 */

/* 循环栈：支持嵌套 for/while 里的 break / continue 回填 */
#define MAX_LOOP 16
#define MAX_LOOP_JMP 32
static int loop_depth;
static int cont_pos[MAX_LOOP][MAX_LOOP_JMP];  /* continue 的 jmp 位置 */
static int cont_n[MAX_LOOP];
static int brk_pos[MAX_LOOP][MAX_LOOP_JMP];   /* break 的 jmp 位置 */
static int brk_n[MAX_LOOP];

/* ---------------- 词法 ---------------- */
enum { T_EOF = 0, T_NUM = 1, T_IDENT = 2, T_STR = 3, T_PUNCT = 4 };

typedef struct {
    int  kind;
    int  num;
    char text[64];
    char str[256];
    int  slen;
} token_t;

static token_t tok;
static const unsigned char *src;
static int src_pos;
static int src_len;

/* 对象式宏表（#define NAME 数字） */
#define MAX_MACROS 160
typedef struct {
    char name[40];
    int  val;
} macro_t;

static macro_t macros[MAX_MACROS];
static int     nmacros;

/* 条件编译：#ifdef / #ifndef / #else / #endif 的嵌套栈 */
#define MAX_COND 16
static int cond_stack[MAX_COND];   /* 1 = 本层条件成立 */
static int cond_taken[MAX_COND];   /* 1 = 已经进入过真分支 */
static int cond_depth;
static int skipping;               /* 1 = 当前行整行丢弃 */

static void recalc_skip(void)
{
    int i;

    skipping = 0;
    for (i = 0; i < cond_depth; i++)
        if (!cond_stack[i])
            skipping = 1;
}

static int macro_lookup(const char *n, int *out)
{
    for (int i = 0; i < nmacros; i++)
        if (strcmp(macros[i].name, n) == 0) {
            *out = macros[i].val;
            return 1;
        }
    return 0;
}

static void die(const char *msg)
{
    printf("tc: error line %d: %s\n", cur_line, msg);
    sys_exit(1);
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_id0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_id(int c) { return is_id0(c) || is_digit(c); }

/* 读一个十进制/十六进制数字（供 #define 表达式求值用） */
static int read_num(void)
{
    int v = 0;

    if (src_pos + 1 < src_len && src[src_pos] == '0' &&
        (src[src_pos + 1] == 'x' || src[src_pos + 1] == 'X')) {
        src_pos += 2;
        while (src_pos < src_len) {
            int h = src[src_pos], d;
            if (h >= '0' && h <= '9')      d = h - '0';
            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
            else break;
            v = v * 16 + d;
            src_pos++;
        }
    } else {
        while (src_pos < src_len && is_digit(src[src_pos])) {
            v = v * 10 + (src[src_pos] - '0');
            src_pos++;
        }
    }
    return v;
}

static void skip_ws(void)
{
    while (src_pos < src_len && (src[src_pos] == ' ' || src[src_pos] == '\t'))
        src_pos++;
}

static void next(void)
{
    int c;

    for (;;) {
        if (src_pos >= src_len)
            break;
        c = src[src_pos];
        if (is_space(c)) {
            if (c == '\n')
                cur_line++;
            src_pos++;
            continue;
        }
        if (c == '/' && src_pos + 1 < src_len && src[src_pos + 1] == '/') {
            while (src_pos < src_len && src[src_pos] != '\n')
                src_pos++;
            continue;
        }
        if (c == '/' && src_pos + 1 < src_len && src[src_pos + 1] == '*') {
            src_pos += 2;
            while (src_pos + 1 < src_len &&
                   !(src[src_pos] == '*' && src[src_pos + 1] == '/')) {
                if (src[src_pos] == '\n')
                    cur_line++;
                src_pos++;
            }
            src_pos += 2;
            continue;
        }
        if (skipping && c != '#') {          /* 条件编译排除区：整行丢弃 */
            while (src_pos < src_len && src[src_pos] != '\n')
                src_pos++;
            if (src_pos < src_len) {
                src_pos++;
                cur_line++;
            }
            continue;
        }
        break;
    }

    tok.kind = T_EOF;
    tok.text[0] = 0;
    tok.slen = 0;
    if (src_pos >= src_len)
        return;

    c = src[src_pos];

    /* 预处理：只支持对象式宏  #define NAME 数字 */
    if (c == '#') {
        char word[64];
        char mname[40];
        int n = 0, neg = 0, val = 0, have_val = 0;

        src_pos++;                                  /* '#' */
        while (src_pos < src_len && (src[src_pos] == ' ' || src[src_pos] == '\t'))
            src_pos++;
        while (src_pos < src_len && is_id(src[src_pos]) && n < 63)
            word[n++] = (char)src[src_pos++];
        word[n] = 0;

        if (strcmp(word, "ifndef") == 0 || strcmp(word, "ifdef") == 0) {
            char cn[40];
            int cnl = 0, cv, want;

            while (src_pos < src_len && (src[src_pos] == ' ' || src[src_pos] == '\t'))
                src_pos++;
            while (src_pos < src_len && is_id(src[src_pos]) && cnl < 39)
                cn[cnl++] = (char)src[src_pos++];
            cn[cnl] = 0;
            want = macro_lookup(cn, &cv);
            if (strcmp(word, "ifndef") == 0)
                want = !want;
            if (cond_depth >= MAX_COND)
                die("#if nesting too deep");
            cond_stack[cond_depth] = want;
            cond_taken[cond_depth] = want;
            cond_depth++;
            recalc_skip();
            goto hash_done;
        }
        if (strcmp(word, "else") == 0) {
            if (cond_depth <= 0)
                die("#else without #if");
            if (cond_taken[cond_depth - 1])
                cond_stack[cond_depth - 1] = 0;
            else {
                cond_stack[cond_depth - 1] = 1;
                cond_taken[cond_depth - 1] = 1;
            }
            recalc_skip();
            goto hash_done;
        }
        if (strcmp(word, "endif") == 0) {
            if (cond_depth > 0)
                cond_depth--;
            recalc_skip();
            goto hash_done;
        }
        if (skipping)
            goto hash_done;                  /* 排除区里的 #define 一律不生效 */

        if (strcmp(word, "define") == 0) {
            while (src_pos < src_len && (src[src_pos] == ' ' || src[src_pos] == '\t'))
                src_pos++;
            n = 0;
            while (src_pos < src_len && is_id(src[src_pos]) && n < 39)
                mname[n++] = (char)src[src_pos++];
            mname[n] = 0;
            while (src_pos < src_len && (src[src_pos] == ' ' || src[src_pos] == '\t'))
                src_pos++;
            if (src_pos < src_len && src[src_pos] == '-') { neg = 1; src_pos++; }
            skip_ws();
            if (src_pos < src_len && src[src_pos] == '(') {
                /* 表达式宏：只支持 ( 数 op 数 [op 数]... )，够驱动里的 (1 << 3) 用 */
                src_pos++;
                skip_ws();
                val = read_num();
                have_val = 1;
                for (;;) {
                    int op = 0, rhs;

                    skip_ws();
                    if (src_pos >= src_len)
                        break;
                    if (src[src_pos] == ')') {
                        src_pos++;
                        break;
                    }
                    if (src[src_pos] == '<' && src_pos + 1 < src_len && src[src_pos + 1] == '<') {
                        op = 1; src_pos += 2;
                    } else if (src[src_pos] == '>' && src_pos + 1 < src_len && src[src_pos + 1] == '>') {
                        op = 2; src_pos += 2;
                    } else if (src[src_pos] == '|') {
                        op = 3; src_pos++;
                    } else if (src[src_pos] == '&') {
                        op = 4; src_pos++;
                    } else if (src[src_pos] == '^') {
                        op = 5; src_pos++;
                    } else if (src[src_pos] == '+') {
                        op = 6; src_pos++;
                    } else if (src[src_pos] == '-') {
                        op = 7; src_pos++;
                    } else {
                        break;
                    }
                    skip_ws();
                    rhs = read_num();
                    if (op == 1)      val = val << rhs;
                    else if (op == 2) val = val >> rhs;
                    else if (op == 3) val = val | rhs;
                    else if (op == 4) val = val & rhs;
                    else if (op == 5) val = val ^ rhs;
                    else if (op == 6) val = val + rhs;
                    else              val = val - rhs;
                }
            } else {
                val = read_num();
                have_val = (val > 0) || (src[src_pos - 1] == '0');
            }
            if (n > 0) {
                if (nmacros >= MAX_MACROS)
                    die("too many #define");
                memcpy(macros[nmacros].name, mname, (unsigned)n + 1);
                macros[nmacros].val = have_val ? (neg ? -val : val) : 0;
                nmacros++;
            }
        }
hash_done:
        while (src_pos < src_len && src[src_pos] != '\n')   /* 吃掉整行 */
            src_pos++;
        if (src_pos < src_len) {
            src_pos++;
            cur_line++;                                      /* 行号跟着走，报错才准 */
        }
        next();                                              /* 继续取下一个 token */
        return;
    }

    if (c == '0' && src_pos + 1 < src_len &&
        (src[src_pos + 1] == 'x' || src[src_pos + 1] == 'X')) {
        int hv = 0;
        src_pos += 2;
        while (src_pos < src_len) {
            int h = src[src_pos];
            int d;
            if (h >= '0' && h <= '9')      d = h - '0';
            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
            else break;
            hv = hv * 16 + d;
            src_pos++;
        }
        tok.kind = T_NUM;
        tok.num = hv;
        return;
    }

    if (is_digit(c)) {
        int v = 0;
        while (src_pos < src_len && is_digit(src[src_pos])) {
            v = v * 10 + (src[src_pos] - '0');
            src_pos++;
        }
        tok.kind = T_NUM;
        tok.num = v;
        return;
    }

    if (is_id0(c)) {
        int n = 0;
        while (src_pos < src_len && is_id(src[src_pos])) {
            if (n < 63)
                tok.text[n++] = (char)src[src_pos];
            src_pos++;
        }
        tok.text[n] = 0;
        {
            int mv;
            if (macro_lookup(tok.text, &mv)) {   /* 宏名 → 数字字面量 */
                tok.kind = T_NUM;
                tok.num = mv;
                return;
            }
        }
        tok.kind = T_IDENT;
        return;
    }

    if (c == '\'') {
        int val = 0;
        src_pos++;                                  /* 开引号 */
        if (src_pos < src_len && src[src_pos] != '\'') {
            int ch = src[src_pos];
            if (ch == '\\' && src_pos + 1 < src_len) {
                src_pos++;
                ch = src[src_pos];
                if (ch == 'n') ch = '\n';
                else if (ch == 't') ch = '\t';
                else if (ch == 'r') ch = '\r';
                else if (ch == '0') ch = 0;
                else if (ch == '\\') ch = '\\';
                else if (ch == '\'') ch = '\'';
                else if (ch == '"') ch = '"';
            }
            val = ch;
            src_pos++;
        }
        if (src_pos < src_len && src[src_pos] == '\'')
            src_pos++;                              /* 闭引号 */
        tok.kind = T_NUM;
        tok.num = val;
        return;
    }

    if (c == '"') {
        int n = 0;
        src_pos++;
        while (src_pos < src_len && src[src_pos] != '"') {
            int ch = src[src_pos];
            if (ch == '\\' && src_pos + 1 < src_len) {
                src_pos++;
                ch = src[src_pos];
                if (ch == 'n') ch = '\n';
                else if (ch == 't') ch = '\t';
            }
            if (n < 255)
                tok.str[n++] = (char)ch;
            src_pos++;
        }
        if (src_pos < src_len)
            src_pos++;
        tok.str[n] = 0;
        tok.slen = n;
        tok.kind = T_STR;
        return;
    }

#define TWO(a, b, t) \
    if (c == (a) && src_pos + 1 < src_len && src[src_pos + 1] == (b)) { \
        src_pos += 2; tok.kind = T_PUNCT; tok.text[0] = (a); tok.text[1] = (b); \
        tok.text[2] = 0; return; }

#define THREE(a, b, d, t) \
    if (c == (a) && src_pos + 2 < src_len && src[src_pos + 1] == (b) && \
        src[src_pos + 2] == (d)) { \
        src_pos += 3; tok.kind = T_PUNCT; tok.text[0] = (a); tok.text[1] = (b); \
        tok.text[2] = (d); tok.text[3] = 0; return; }

    THREE('<', '<', '=', "<<=")
    THREE('>', '>', '=', ">>=")
    TWO('=', '=', "==")
    TWO('!', '=', "!=")
    TWO('<', '<', "<<")
    TWO('<', '=', "<=")
    TWO('>', '>', ">>")
    TWO('>', '=', ">=")
    TWO('&', '&', "&&")
    TWO('&', '=', "&=")
    TWO('|', '|', "||")
    TWO('|', '=', "|=")
    TWO('^', '=', "^=")
    TWO('+', '=', "+=")
    TWO('-', '=', "-=")
    TWO('*', '=', "*=")
    TWO('/', '=', "/=")
    TWO('%', '=', "%=")
    TWO('+', '+', "++")
    TWO('-', '-', "--")
    TWO('-', '>', "->")
#undef TWO
#undef THREE

    src_pos++;
    tok.kind = T_PUNCT;
    tok.text[0] = (char)c;
    tok.text[1] = 0;
}

static int is_punct(const char *s) { return tok.kind == T_PUNCT && strcmp(tok.text, s) == 0; }
static int is_word(const char *s) { return tok.kind == T_IDENT && strcmp(tok.text, s) == 0; }

/* ---------------- typedef 别名表 ---------------- */
#define MAX_TALIAS 32
typedef struct {
    char name[40];
    int  size;
    int  struct_id;
    int  is_ptr;
} talias_t;

static talias_t talias[MAX_TALIAS];
static int      ntalias;

static int talias_id_of(const char *name)
{
    int i;

    for (i = 0; i < ntalias; i++)
        if (strcmp(talias[i].name, name) == 0)
            return i;
    return -1;
}

static void talias_add(const char *name, int size, int sid, int is_ptr)
{
    if (ntalias >= MAX_TALIAS)
        die("too many typedefs");
    strncpy(talias[ntalias].name, name, 39);
    talias[ntalias].name[39] = 0;
    talias[ntalias].size = size;
    talias[ntalias].struct_id = sid;
    talias[ntalias].is_ptr = is_ptr;
    ntalias++;
}

/* 吃掉类型说明符，返回该类型占几个字节。
   标量一律按 4 字节分配（简化实现）；只有数组元素才按真实宽度步进。 */
static int skip_type_prefix(void)
{
    int size = 4;
    int uns = 0;

    last_type_struct = -1;
    for (;;) {
        if (is_word("unsigned")) { uns = 1; next(); continue; }
        if (is_word("signed"))   { uns = 0; next(); continue; }
        if (is_word("static") || is_word("const") || is_word("volatile") ||
            is_word("extern") || is_word("register") || is_word("inline")) {
            next();
            continue;
        }
        if (is_word("char"))  { size = 1; next(); continue; }
        if (is_word("short")) { size = 2; next(); continue; }
        if (is_word("long"))  { size = 4; next(); continue; }
        if (is_word("int"))   { size = 4; next(); continue; }
        if (is_word("void"))  { size = 4; next(); continue; }
        if (tok.kind == T_IDENT) {
            int ti = talias_id_of(tok.text);

            if (ti >= 0) {
                size = talias[ti].size;
                last_type_struct = talias[ti].struct_id;
                last_type_unsigned = 0;
                next();
                return size;
            }
        }
        break;
    }
    last_type_unsigned = uns;
    return size;
}


static void expect_punct(const char *s)
{
    if (!is_punct(s)) {
        printf("tc: expect '%s' got kind=%d text='%s' line %d\n",
               s, tok.kind, tok.text, cur_line);
        die("expected punctuator");
    }
    next();
}

/* ---------------- 发射 ---------------- */
static void e8(int v)
{
    if (code_len >= CODE_MAX - 8)
        die("code buffer overflow");
    code[code_len++] = (unsigned char)(v & 0xFF);
}

static int e32(int v)
{
    int p = code_len;
    e8(v); e8(v >> 8); e8(v >> 16); e8(v >> 24);
    return p;
}

static void patch32(int pos, int v)
{
    code[pos] = (unsigned char)(v & 0xFF);
    code[pos + 1] = (unsigned char)((v >> 8) & 0xFF);
    code[pos + 2] = (unsigned char)((v >> 16) & 0xFF);
    code[pos + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static int je_placeholder(int cc_opcode)
{
    e8(0x0F);
    e8(cc_opcode);
    return e32(0);
}

static int emit_jmp32(void)
{
    e8(0xE9);
    return e32(0);
}

static void patch_rel(int pos, int target)
{
    patch32(pos, target - (pos + 4));
}

/* ---------------- 字符串池 ---------------- */
static char strs[STR_MAX];
static int  strs_len;
static int  str_off[MAX_STR];
static int  str_len_v[MAX_STR];
static int  str_count;

static int add_string(const char *s, int len)
{
    int i;
    for (i = 0; i < str_count; i++)
        if (str_len_v[i] == len && strcmp(&strs[str_off[i]], s) == 0)
            return i;
    if (str_count >= MAX_STR)
        die("too many string literals");
    if (strs_len + len + 1 >= STR_MAX)
        die("string pool overflow");
    str_off[str_count] = strs_len;
    str_len_v[str_count] = len;
    if (len)
        memcpy(&strs[strs_len], s, (unsigned)len);
    strs[strs_len + len] = 0;
    strs_len += len + 1;
    return str_count++;
}

static int str_patch[MAX_PATCH];
static int str_patch_id[MAX_PATCH];
static int str_patch_n;

static void ref_string(int id)
{
    int pos;
    e8(0xB8);                       /* mov eax, imm32 */
    pos = e32(0);
    if (str_patch_n >= MAX_PATCH)
        die("too many string references");
    str_patch[str_patch_n] = pos;
    str_patch_id[str_patch_n] = id;
    str_patch_n++;
}

/* ---------------- 函数表 ---------------- */
typedef struct {
    char name[48];
    int  nparams;
    int  off;
} func_t;

static func_t funcs[MAX_FUNCS];
static int    nfuncs;


static int  call_patch[MAX_PATCH];
static char call_name[MAX_PATCH][48];
static int  call_patch_n;

static int func_find(const char *name)
{
    for (int i = 0; i < nfuncs; i++)
        if (strcmp(funcs[i].name, name) == 0)
            return i;
    return -1;
}

static int func_add(const char *name, int nparams)
{
    int f = func_find(name);
    if (f >= 0) {
        if (funcs[f].off >= 0)
            die("duplicate function");
        funcs[f].nparams = nparams;
        return f;
    }
    if (nfuncs >= MAX_FUNCS)
        die("too many functions");
    f = nfuncs++;
    strncpy(funcs[f].name, name, sizeof(funcs[f].name) - 1);
    funcs[f].nparams = nparams;
    funcs[f].off = -1;
    return f;
}

/* ---------------- 变量表 ---------------- */
/* ---------------- 结构体类型表 ---------------- */
typedef struct {
    char sname[24];
    int  nf;
    char fname[MAX_FIELDS][20];
    int  foff[MAX_FIELDS];
    int  fsize[MAX_FIELDS];
    int  fcount[MAX_FIELDS];   /* 0 = 标量；>0 = 数组成员个数 */
    int  f_sid[MAX_FIELDS];    /* >=0 = 内嵌结构体类型 id / 指针字段的目标类型 */
    int  f_ptr[MAX_FIELDS];    /* 1 = 字段是 struct T *（指针） */
    int  size;
} struct_t;

static struct_t structs[MAX_STRUCTS];
static int      nstructs;

static int struct_id_of(const char *name)
{
    for (int i = 0; i < nstructs; i++)
        if (strcmp(structs[i].sname, name) == 0)
            return i;
    return -1;
}

static int field_index(struct_t *s, const char *fname)
{
    for (int i = 0; i < s->nf; i++)
        if (strcmp(s->fname[i], fname) == 0)
            return i;
    return -1;
}

typedef struct {
    char name[48];
    int  off;
    int  is_array;
    int  count;
    int  is_global;
    int  gaddr;
    int  struct_id;      /* -1 = 普通；>=0 = 该结构体的实例或指针 */
    int  is_struct_ptr;  /* 1 = 指向结构体的指针（可用 q->x） */
    int  elem_size;      /* 数组元素字节数：char[]=1, short[]=2, 其他=4 */
    int  ptr_elem;       /* >0 表示指针，值为指向元素的字节宽度 */
    int  is_unsigned;    /* 1 = 声明为 unsigned，>> 生成 shr 而不是 sar */
} var_t;

static var_t vars[MAX_VARS];
static int   nvars;
static int   locals;
static int   prologue_pos;

static var_t gvars[MAX_GVARS];
static int   ngvars;
static unsigned char gdata[GDATA_MAX];
static int   gdata_len;

/* ---------------- 全局初值 ---------------- */
#define MAX_GINIT  256

typedef struct {
    int addr;      /* gdata 内偏移 */
    int size;      /* 1 / 2 / 4 */
    int val;
} ginit_t;

static ginit_t ginit[MAX_GINIT];
static int     nginit;

/* 只认编译期常量：数字、-数字、+数字、宏常量 */
static int const_expr_simple(void)
{
    int neg = 0, v = 0, mv;

    if (is_punct("-")) { next(); neg = 1; }
    else if (is_punct("+")) { next(); }
    if (tok.kind == T_NUM) {
        v = tok.num;
        next();
    } else if (tok.kind == T_IDENT && macro_lookup(tok.text, &mv)) {
        v = mv;
        next();
    } else {
        die("global initializer must be a compile-time constant");
    }
    return neg ? -v : v;
}

/* 先查局部，再查全局 */
static var_t *var_lookup(const char *name)
{
    for (int i = nvars - 1; i >= 0; i--)
        if (strcmp(vars[i].name, name) == 0)
            return &vars[i];
    for (int i = ngvars - 1; i >= 0; i--)
        if (strcmp(gvars[i].name, name) == 0)
            return &gvars[i];
    return (var_t *)0;
}

/* 全局变量分配（只支持 BSS，不支持初值） */
static int global_add(const char *name, int size, int is_array, int count)
{
    int off;
    var_t *v;

    if (ngvars >= MAX_GVARS)
        die("too many global variables");
    off = (gdata_len + 3) & ~3;
    if (off + size > GDATA_MAX)
        die("global data too large");
    gdata_len = off + size;
    memset(gdata + off, 0, (unsigned)size);

    v = &gvars[ngvars++];
    memset(v, 0, sizeof(*v));
    strncpy(v->name, name, sizeof(v->name) - 1);
    v->is_array = is_array;
    v->count = count;
    v->is_global = 1;
    v->gaddr = off;
    v->struct_id = -1;
    v->is_struct_ptr = 0;
    v->elem_size = 4;
    v->ptr_elem = 0;
    return off;
}

/* eax(reg=0) / edx(reg=2) = 变量地址 */
static void emit_addr(int reg, const var_t *v)
{
    if (v->is_global) {
        e8(0x8D); e8(0x05 | (reg << 3)); e32(CODE_BASE + GC_BASE + v->gaddr);
    } else {
        e8(0x8D); e8(0x85 | (reg << 3)); e32(v->off);
    }
}

/* eax = 变量当前值 */
static void emit_load(const var_t *v)
{
    if (v->is_global) { e8(0x8B); e8(0x05); e32(CODE_BASE + GC_BASE + v->gaddr); }
    else              { e8(0x8B); e8(0x85); e32(v->off); }
}

/* 变量 = eax */
static void emit_store(const var_t *v)
{
    if (v->is_global) { e8(0x89); e8(0x05); e32(CODE_BASE + GC_BASE + v->gaddr); }
    else              { e8(0x89); e8(0x85); e32(v->off); }
}

/* 吃掉 '.' 或 '->' 与字段名，返回字段下标 */
static int field_take(struct_t *s)
{
    int idx;

    next();
    if (tok.kind != T_IDENT)
        die("expected a field name");
    idx = field_index(s, tok.text);
    if (idx < 0)
        die("unknown field");
    next();
    return idx;
}

/* 当前表达式结果的"指针元素宽度"：0 = 不是指针。
   gen_add / gen_unary 用它决定要不要按元素宽度缩放或按宽度解引用。 */
static int expr_ptr_elem;

static void gen_expr(void);

/* 结构体成员地址 -> edx；支持 q->x / a.b.c / p.v[i] / p.next->y */
static void gen_member_addr(const var_t *vp)
{
    int sid = vp->struct_id;
    int can_arrow = vp->is_struct_ptr;

    if (vp->is_struct_ptr) {
        if (vp->is_global) { e8(0x8B); e8(0x15); e32(CODE_BASE + GC_BASE + vp->gaddr); }
        else               { e8(0x8B); e8(0x95); e32(vp->off); }
    } else {
        emit_addr(2, vp);
    }

    for (;;) {
        struct_t *s;
        int idx;

        if (!is_punct(".") && !is_punct("->"))
            break;
        if (is_punct("->") && !can_arrow)
            die("'->' needs a struct pointer on its left");
        if (sid < 0)
            die("member access on something that is not a struct");
        s = &structs[sid];
        idx = field_take(s);
        if (s->foff[idx])
            { e8(0x8D); e8(0x92); e32(s->foff[idx]); }     /* lea edx,[edx+foff] */

        if (is_punct("[")) {
            int stride;
            if (!s->fcount[idx])
                die("subscript on a field that is not an array");
            stride = s->fsize[idx] / s->fcount[idx];
            e8(0x52);                                   /* push edx */
            next();
            gen_expr();
            expect_punct("]");
            if (stride == 4)      { e8(0xC1); e8(0xE0); e8(0x02); }
            else if (stride == 2) { e8(0xC1); e8(0xE0); e8(0x01); }
            else if (stride == 8) { e8(0xC1); e8(0xE0); e8(0x03); }
            else                  { e8(0x69); e8(0xC0); e32(stride); }
            e8(0x59);                                   /* pop ecx */
            e8(0x01); e8(0xC1);                         /* add ecx,eax */
            e8(0x89); e8(0xCA);                         /* mov edx,ecx */
            break;
        }

        if (s->f_ptr[idx]) {
            /* struct T * 字段：后面还接 -> 才解引用；否则它就是成员本身（赋值要它的地址） */
            if (is_punct("->")) {
                e8(0x8B); e8(0x12);                     /* mov edx,[edx] */
                sid = s->f_sid[idx];
                can_arrow = 1;
                continue;
            }
            break;
        }
        if (s->f_sid[idx] >= 0) {                       /* 内嵌结构体：继续 . 链 */
            sid = s->f_sid[idx];
            can_arrow = 0;
            continue;
        }
        break;
    }
}

/* 成员读：地址 -> edx，然后 mov eax,[edx] */
static void gen_member_read(const var_t *vp)
{
    gen_member_addr(vp);
    e8(0x8B); e8(0x02);                                 /* mov eax,[edx] */
}

static int var_decl_ptr_elem;
static int var_decl_struct_id = -1;   /* 声明中的结构体类型 id */
static int var_decl_is_struct_ptr;    /* 声明的是否 struct 指针 */      /* 声明解析时临时传递"指针指向宽度" */

static int var_add(const char *name, int size)
{
    var_t *v;
    if (nvars >= MAX_VARS)
        die("too many variables");
    if (locals + size > 65536)
        die("locals too large");
    locals += (size + 3) & ~3;
    v = &vars[nvars];
    strncpy(v->name, name, sizeof(v->name) - 1);
    v->off = -locals;
    v->is_array = 0;
    v->count = 1;
    v->is_global = 0;
    v->gaddr = 0;
    v->struct_id = -1;
    v->is_struct_ptr = 0;
    v->elem_size = 4;
    v->ptr_elem = 0;
    v->is_unsigned = 0;
    nvars++;
    return v->off;
}

static void var_add_param(const char *name, int off)
{
    var_t *v;
    if (nvars >= MAX_VARS)
        die("too many variables");
    v = &vars[nvars];
    strncpy(v->name, name, sizeof(v->name) - 1);
    v->off = off;
    v->is_array = 0;
    v->count = 1;
    v->is_global = 0;
    v->gaddr = 0;
    v->struct_id = -1;
    v->is_struct_ptr = 0;
    v->elem_size = 4;
    v->ptr_elem = 0;
    nvars++;
}

static void var_mark_array(int idx, int count)
{
    vars[idx].is_array = 1;
    vars[idx].count = count;
}

/* ---------------- 表达式 ---------------- */
static void gen_expr(void);

static void gen_call(const char *name)
{
    int f;

    if (strcmp(name, "sys4") == 0) {
        expect_punct("(");
        gen_expr(); e8(0x50);
        expect_punct(",");
        gen_expr(); e8(0x50);
        expect_punct(",");
        gen_expr(); e8(0x50);
        expect_punct(",");
        gen_expr(); e8(0x50);
        expect_punct(")");
        e8(0x5A);                     /* pop edx */
        e8(0x59);                     /* pop ecx */
        e8(0x5B);                     /* pop ebx */
        e8(0x58);                     /* pop eax */
        e8(0xCD); e8(0x80);
        return;
    }

    if (strcmp(name, "printn") == 0) {
        /* printn(x)：sys4(SYS_PRINTN=50, x, 0, 0) */
        expect_punct("(");
        gen_expr();                      /* eax = 值 */
        e8(0x89); e8(0xC3);              /* mov ebx,eax */
        e8(0xB8); e32(60);               /* mov eax,60 */
        e8(0x31); e8(0xC9);              /* xor ecx,ecx */
        e8(0x89); e8(0xCA);              /* mov edx,ecx */
        e8(0xCD); e8(0x80);              /* int 0x80 */
        expect_punct(")");
        return;
    }

    f = func_find(name);
    if (f < 0)
        die("call to an undeclared function");

    expect_punct("(");
    {
        int n = 0;
        if (!is_punct(")")) {
            for (;;) {
                gen_expr();
                e8(0x50);
                n++;
                if (is_punct(",")) { next(); continue; }
                break;
            }
        }
        expect_punct(")");
        if (n != funcs[f].nparams)
            die("argument count mismatch");
    }

    e8(0xE8);
    if (call_patch_n >= MAX_PATCH)
        die("too many calls");
    call_patch[call_patch_n] = e32(0);
    strncpy(call_name[call_patch_n], name, sizeof(call_name[0]) - 1);
    call_patch_n++;

    if (funcs[f].nparams) {
        e8(0x83); e8(0xC4);
        e8(funcs[f].nparams * 4);
    }
}

static void gen_primary(void)
{
    expr_ptr_elem = 0;
    expr_unsigned = 0;

    if (tok.kind == T_NUM) {
        int v = tok.num;
        next();
        e8(0xB8);
        e32(v);
        return;
    }

    if (tok.kind == T_STR) {
        int id = add_string(tok.str, tok.slen);
        next();
        ref_string(id);
        return;
    }

    if (tok.kind == T_IDENT) {
        char name[48];
        var_t *vp;

        strncpy(name, tok.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();

        if (is_punct("(")) {
            gen_call(name);
            return;
        }

        vp = var_lookup(name);
        if (!vp)
            die("undefined variable");

        /* 记下这个变量作为表达式时的指针宽度 */
        if (vp->is_array)
            expr_ptr_elem = vp->elem_size;
        else if (vp->ptr_elem > 0)
            expr_ptr_elem = vp->ptr_elem;
        else
            expr_ptr_elem = 0;
        expr_unsigned = vp->is_unsigned;

        if (is_punct(".") || is_punct("->")) {
            gen_member_read(vp);
            return;
        }

        if (is_punct("[")) {
            int stride = 4;
            next();
            gen_expr();
            expect_punct("]");
            if (vp->struct_id >= 0 && vp->is_array)
                stride = structs[vp->struct_id].size;
            else if (vp->ptr_elem > 0)
                stride = vp->ptr_elem;
            else if (vp->is_array && vp->elem_size > 0)
                stride = vp->elem_size;
            if (stride == 2)      { e8(0xC1); e8(0xE0); e8(0x01); }   /* shl eax,1  */
            else if (stride == 4) { e8(0xC1); e8(0xE0); e8(0x02); }   /* shl eax,2  */
            else if (stride == 8) { e8(0xC1); e8(0xE0); e8(0x03); }   /* shl eax,3  */
            else                  { e8(0x69); e8(0xC0); e32(stride); } /* imul eax,imm32 */
            if (vp->is_array) {
                emit_addr(2, vp);                    /* edx = 数组基址 */
            } else if (vp->is_global) {
                e8(0x8B); e8(0x15);
                e32(CODE_BASE + GC_BASE + vp->gaddr);   /* edx = 指针值 */
            } else {
                e8(0x8B); e8(0x95); e32(vp->off);        /* edx = 指针值 */
            }
            e8(0x01); e8(0xC2);                      /* add edx,eax */
            if (vp->struct_id >= 0 && vp->is_array && is_punct(".")) {
                /* a[i].f ：edx 已是元素地址 */
                struct_t *s = &structs[vp->struct_id];
                int idx = field_take(s);
                e8(0x8B); e8(0x82); e32(s->foff[idx]);   /* mov eax,[edx+foff] */
                return;
            }
            if (stride == 1)      { e8(0x0F); e8(0xB6); e8(0x02); }   /* movzx eax, byte [edx] */
            else if (stride == 2) { e8(0x0F); e8(0xBF); e8(0x02); }   /* movsx eax, word [edx] */
            else                  { e8(0x8B); e8(0x02); }            /* mov eax,[edx] */
            return;
        }

        if (vp->is_array) {
            emit_addr(0, vp);                        /* eax = 数组地址 */
            return;
        }
        emit_load(vp);                               /* eax = 变量值 */
        return;
    }

    if (is_punct("(")) {
        next();
        gen_expr();
        expect_punct(")");
        return;
    }

    die("bad expression");
}

/* 试探 "(类型)" 或 "(类型 *)" 形式；成功则吃掉右括号并返回 1。
 * 失败时恢复词法位置，交给 gen_primary 当普通括号表达式处理。 */
static int cast_word_is_type(void)
{
    return is_word("int") || is_word("char") || is_word("short") ||
           is_word("long") || is_word("unsigned") || is_word("signed") ||
           is_word("void") || is_word("const") ||
           (tok.kind == T_IDENT && talias_id_of(tok.text) >= 0);
}

static int try_cast(void)
{
    int save_pos = src_pos;
    int save_line = cur_line;
    token_t save_tok = tok;
    int tsz, stars = 0;

    if (!is_punct("("))
        return 0;
    next();
    if (!cast_word_is_type()) {
        src_pos = save_pos; cur_line = save_line; tok = save_tok;
        return 0;
    }
    tsz = skip_type_prefix();
    while (is_punct("*")) {
        next();
        stars++;
    }
    if (!is_punct(")")) {                       /* 不是转换，回退 */
        src_pos = save_pos; cur_line = save_line; tok = save_tok;
        return 0;
    }
    next();
    expr_ptr_elem = (stars > 0) ? tsz : 0;
    return 1;
}

static void gen_unary(void)
{
    if (is_punct("-")) {
        next();
        gen_unary();
        e8(0xF7); e8(0xD8);              /* neg eax */
        return;
    }
    if (is_punct("!")) {
        next();
        gen_unary();
        e8(0x85); e8(0xC0);              /* test eax,eax */
        e8(0x0F); e8(0x94); e8(0xC0);    /* sete al */
        e8(0x0F); e8(0xB6); e8(0xC0);    /* movzx eax,al */
        return;
    }
    if (is_word("sizeof")) {
        int sz = 4;

        next();
        if (!is_punct("("))
            die("sizeof needs a type in parentheses");
        next();
        if (is_word("struct")) {
            char nm[48];
            int i, id = -1;

            next();
            if (tok.kind != T_IDENT)
                die("sizeof(struct ...) needs a name");
            strncpy(nm, tok.text, 46);
            nm[46] = 0;
            next();
            for (i = 0; i < nstructs; i++)
                if (strcmp(structs[i].sname, nm) == 0) { id = i; break; }
            if (id < 0)
                die("sizeof: unknown struct");
            sz = structs[id].size;
        } else if (tok.kind == T_IDENT && var_lookup(tok.text)) {
            var_t *vp = var_lookup(tok.text);

            if (vp->is_array)
                sz = vp->count * vp->elem_size;
            else if (vp->is_struct_ptr || vp->ptr_elem > 0)
                sz = 4;
            else if (vp->struct_id >= 0)
                sz = structs[vp->struct_id].size;
            else
                sz = 4;
            next();
        } else {
            sz = skip_type_prefix();
            if (is_punct("*")) { next(); sz = 4; }
        }
        expect_punct(")");
        e8(0xB8); e32(sz);               /* mov eax, imm32 */
        return;
    }
    if (is_punct("~")) {                 /* 按位取反 */
        next();
        gen_unary();
        e8(0xF7); e8(0xD0);              /* not eax */
        return;
    }
    if (is_punct("*")) {                 /* 解引用：按指向元素的宽度取值 */
        int w;
        next();
        gen_unary();
        w = expr_ptr_elem;
        if (w == 1) {
            e8(0x0F); e8(0xB6); e8(0x00);  /* movzx eax, byte [eax] */
        } else if (w == 2) {
            e8(0x0F); e8(0xBF); e8(0x00);  /* movsx eax, word [eax] */
        } else {
            e8(0x8B); e8(0x00);            /* mov eax,[eax] */
        }
        expr_ptr_elem = 0;
        return;
    }
    if (is_punct("&")) {                 /* 取地址 */
        char name[48];
        var_t *vp;

        next();
        if (tok.kind != T_IDENT)
            die("'&' needs a variable");
        strncpy(name, tok.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();
        vp = var_lookup(name);
        if (!vp)
            die("address of an undefined variable");
        emit_addr(0, vp);                /* eax = 变量地址 */
        return;
    }
    if (is_punct("*")) {             /* *p 读：按元素宽度取值 */
        int w;

        next();
        gen_unary();                 /* eax = 指针值；expr_ptr_elem = 元素宽度 */
        w = expr_ptr_elem;
        if (w == 1)      { e8(0x0F); e8(0xB6); e8(0x00); }   /* movzx eax,byte [eax] */
        else if (w == 2) { e8(0x0F); e8(0xB7); e8(0x00); }   /* movzx eax,word [eax] */
        else             { e8(0x8B); e8(0x00); }              /* mov eax,[eax] */
        expr_ptr_elem = 0;
        return;
    }
    if (is_punct("(") && try_cast()) {
        gen_unary();                 /* 转换后的表达式，值本来就是 eax */
        return;
    }
    gen_primary();
}

static void gen_mul(void)
{
    gen_unary();
    while (is_punct("*") || is_punct("/") || is_punct("%")) {
        char op = tok.text[0];
        next();
        e8(0x50);                        /* push 左值 */
        gen_unary();
        e8(0x89); e8(0xC1);              /* mov ecx,eax  (右值) */
        e8(0x58);                        /* pop eax      (左值) */
        if (op == '*') {
            e8(0x0F); e8(0xAF); e8(0xC1);        /* imul eax,ecx */
        } else if (op == '/') {
            e8(0x99);                            /* cdq */
            e8(0xF7); e8(0xF9);                  /* idiv ecx */
        } else {
            e8(0x99);                            /* cdq */
            e8(0xF7); e8(0xF9);                  /* idiv ecx */
            e8(0x89); e8(0xD0);                  /* mov eax,edx */
        }
    }
}

static void gen_add(void)
{
    gen_mul();
    while (is_punct("+") || is_punct("-")) {
        char op = tok.text[0];
        int step = expr_ptr_elem;          /* 左操作数是指针时按元素宽度缩放 */

        next();
        e8(0x50);                          /* push 左值 */
        gen_mul();                         /* eax = 右值 */
        if (step > 0 && step != 1) {
            e8(0x69); e8(0xC0); e32(step); /* imul eax, step */
        }
        e8(0x89); e8(0xC1);                /* mov ecx,eax */
        e8(0x58);                          /* pop eax */
        if (op == '+') { e8(0x01); e8(0xC8); }
        else           { e8(0x29); e8(0xC8); }
        expr_ptr_elem = step;              /* 结果仍然是指针 */
    }
}

static void gen_shift(void)
{
    int u;

    gen_add();
    u = expr_unsigned;                   /* 左操作数是否无符号 */
    while (is_punct("<<") || is_punct(">>")) {
        int left_shift = (tok.text[0] == '<');

        next();
        e8(0x50);                        /* push 左值 */
        gen_add();                       /* eax = 移位量 */
        e8(0x89); e8(0xC1);              /* mov ecx,eax */
        e8(0x58);                        /* pop eax */
        if (left_shift)     { e8(0xD3); e8(0xE0); }   /* shl eax,cl */
        else if (u)         { e8(0xD3); e8(0xE8); }   /* shr eax,cl（无符号） */
        else                { e8(0xD3); e8(0xF8); }   /* sar eax,cl（有符号） */
        expr_unsigned = u;
    }
}

static void gen_rel(void)
{
    gen_shift();
    while (is_punct("<") || is_punct(">") || is_punct("<=") || is_punct(">=")) {
        char op0 = tok.text[0];
        char op1 = tok.text[1];
        int cc;

        next();
        e8(0x50);
        gen_shift();
        e8(0x89); e8(0xC1);              /* mov ecx,eax */
        e8(0x58);                        /* pop eax (左值) */
        e8(0x39); e8(0xC8);              /* cmp eax,ecx */
        if (op1 == '=')
            cc = (op0 == '<') ? 0x9E : 0x9D;
        else
            cc = (op0 == '<') ? 0x9C : 0x9F;
        e8(0x0F); e8(cc); e8(0xC0);
        e8(0x0F); e8(0xB6); e8(0xC0);
    }
}

static void gen_eq(void)
{
    gen_rel();
    while (is_punct("==") || is_punct("!=")) {
        int cc = (tok.text[0] == '=') ? 0x94 : 0x95;
        next();
        e8(0x50);
        gen_rel();
        e8(0x89); e8(0xC1);
        e8(0x58);                        /* pop eax (左值) */
        e8(0x39); e8(0xC8);
        e8(0x0F); e8(cc); e8(0xC0);
        e8(0x0F); e8(0xB6); e8(0xC0);
    }
}

static void gen_band(void)
{
    gen_eq();
    while (is_punct("&")) {
        next();
        e8(0x50);
        gen_eq();
        e8(0x89); e8(0xC1);
        e8(0x58);
        e8(0x21); e8(0xC8);              /* and eax,ecx */
    }
}

static void gen_bxor(void)
{
    gen_band();
    while (is_punct("^")) {
        next();
        e8(0x50);
        gen_band();
        e8(0x89); e8(0xC1);
        e8(0x58);
        e8(0x31); e8(0xC8);              /* xor eax,ecx */
    }
}

static void gen_bor(void)
{
    gen_bxor();
    while (is_punct("|")) {
        next();
        e8(0x50);
        gen_bxor();
        e8(0x89); e8(0xC1);
        e8(0x58);
        e8(0x09); e8(0xC8);              /* or eax,ecx */
    }
}

static void gen_and(void)
{
    gen_bor();
    while (is_punct("&&")) {
        next();
        e8(0x50);
        gen_eq();
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x95); e8(0xC0);
        e8(0x0F); e8(0xB6); e8(0xC0);
        e8(0x89); e8(0xC1);
        e8(0x58);
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x95); e8(0xC0);
        e8(0x0F); e8(0xB6); e8(0xC0);
        e8(0x21); e8(0xC8);              /* and eax,ecx */
    }
}

static void gen_or(void)
{
    gen_and();
    while (is_punct("||")) {
        next();
        e8(0x50);
        gen_and();
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x95); e8(0xC0);
        e8(0x0F); e8(0xB6); e8(0xC0);
        e8(0x89); e8(0xC1);
        e8(0x58);
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x95); e8(0xC0);
        e8(0x0F); e8(0xB6); e8(0xC0);
        e8(0x09); e8(0xC8);              /* or eax,ecx */
    }
}

static void gen_expr(void)
{
    gen_or();

    if (is_punct("?")) {                 /* cond ? a : b */
        int jz, jmp2;

        next();
        e8(0x85); e8(0xC0);              /* test eax,eax —— je 依赖 ZF，必须先设标志 */
        jz = je_placeholder(0x84);       /* cond == 0 跳到假分支 */
        gen_expr();                      /* 真值 -> eax */
        jmp2 = emit_jmp32();             /* 跳过假分支 */
        patch_rel(jz, code_len);         /* 假分支入口 */
        expect_punct(":");
        gen_expr();                      /* 假值 -> eax */
        patch_rel(jmp2, code_len);       /* 汇合点：两个分支都只把值留在 eax，不碰栈 */
    }
}

/* ---------------- 语句 ---------------- */
static void gen_stmt(void);
/* ------------------------- 新增：typedef / enum / switch ------------------------- */

/* 定义结构体主体。进入时 tok 停在 '{'，返回后停在 '}' 之后（';' 由调用者吃）。 */
static int struct_def_body(const char *sname)
{
    struct_t *s;
    int size = 0;

    expect_punct("{");
    if (nstructs >= MAX_STRUCTS)
        die("too many structs");
    s = &structs[nstructs];
    memset(s, 0, sizeof(*s));
    strncpy(s->sname, sname, sizeof(s->sname) - 1);
    nstructs++;              /* 先注册：字段里允许引用自身（链表） */
    while (!is_punct("}")) {
        int fsize, fcount = 0, fsid = -1, fptr = 0;

        if (is_word("int")) {
            next();
            fsize = 4;
        } else if (is_word("unsigned") || is_word("signed") || is_word("long") ||
                   is_word("short") || is_word("char")) {
            int tsz = skip_type_prefix();

            fsize = (tsz < 4) ? 4 : tsz;
        } else if (is_word("struct")) {
            char tn[24];
            int tsid;

            next();
            if (tok.kind != T_IDENT)
                die("expected a struct name");
            strncpy(tn, tok.text, sizeof(tn) - 1);
            tn[sizeof(tn) - 1] = 0;
            next();
            tsid = struct_id_of(tn);
            if (tsid < 0)
                die("unknown struct type");
            if (is_punct("*")) {
                next();
                fptr = 1;
                fsid = tsid;
                fsize = 4;
            } else {
                fsid = tsid;
                fsize = structs[tsid].size;
            }
        } else {
            int ti = (tok.kind == T_IDENT) ? talias_id_of(tok.text) : -1;

            if (ti >= 0) {
                fsid = talias[ti].struct_id;
                fsize = talias[ti].size;
                if (talias[ti].is_ptr) { fptr = 1; fsize = 4; }
                next();
                if (is_punct("*")) { next(); fptr = 1; fsize = 4; }
            } else {
                die("only int / unsigned / struct fields are supported");
            }
        }

        if (tok.kind != T_IDENT)
            die("expected a field name");
        if (s->nf >= MAX_FIELDS)
            die("too many fields");
        strncpy(s->fname[s->nf], tok.text, sizeof(s->fname[0]) - 1);
        next();
        if (is_punct("[")) {
            int cnt;

            next();
            if (tok.kind != T_NUM)
                die("array size must be a constant");
            cnt = tok.num;
            if (cnt <= 0 || cnt > 64)
                die("bad array size");
            next();
            expect_punct("]");
            fcount = cnt;
            fsize *= cnt;
        }
        s->foff[s->nf] = size;
        s->fsize[s->nf] = fsize;
        s->fcount[s->nf] = fcount;
        s->f_sid[s->nf] = fsid;
        s->f_ptr[s->nf] = fptr;
        size += fsize;
        s->nf++;
        expect_punct(";");
    }
    next();                              /* '}' */
    s->size = size;
    return nstructs - 1;
}

/* enum 常量值：支持 数字 / -数字（宏名已在词法层替换为数字） */
static int enum_val(void)
{
    int neg = 0, v;

    if (is_punct("-")) { neg = 1; next(); }
    if (tok.kind != T_NUM)
        die("enum value must be a constant");
    v = tok.num;
    next();
    return neg ? -v : v;
}

/* enum Tag { A, B = 5, C }; —— 成员直接进宏表，可当常量使用 */
static void gen_enum(void)
{
    int val = 0;

    next();                              /* enum */
    if (tok.kind == T_IDENT)             /* 有 tag 就吃掉 */
        next();
    expect_punct("{");
    while (!is_punct("}")) {
        char mname[40];

        if (tok.kind != T_IDENT)
            die("expected an enumerator name");
        strncpy(mname, tok.text, 39);
        mname[39] = 0;
        next();
        if (is_punct("=")) {
            next();
            val = enum_val();
        }
        if (nmacros >= MAX_MACROS)
            die("too many enum constants");
        strncpy(macros[nmacros].name, mname, 39);
        macros[nmacros].name[39] = 0;
        macros[nmacros].val = val;
        nmacros++;
        val++;
        if (is_punct(",")) {
            next();
            continue;
        }
        if (!is_punct("}"))
            die("expected ',' or '}' in enum");
    }
    next();                              /* '}' */
    expect_punct(";");
}

/* typedef unsigned int u32;   typedef struct Foo Foo2;   typedef struct { ... } X; */
static void gen_typedef(void)
{
    char aname[48];
    int sz = 4, sid = -1, is_ptr = 0;

    next();                              /* typedef */
    if (is_word("struct")) {
        char sname[24];

        next();
        if (tok.kind != T_IDENT)
            die("expected a struct name");
        strncpy(sname, tok.text, sizeof(sname) - 1);
        sname[sizeof(sname) - 1] = 0;
        next();
        if (is_punct("{"))
            sid = struct_def_body(sname);
        else {
            sid = struct_id_of(sname);
            if (sid < 0)
                die("typedef: unknown struct type");
        }
        if (is_punct("*")) { next(); is_ptr = 1; }
    } else {
        sz = skip_type_prefix();
        if (last_type_struct >= 0) {
            sid = last_type_struct;
            if (is_punct("*")) { next(); is_ptr = 1; }
        } else if (is_punct("*")) {
            next();
            sz = 4;
            is_ptr = 1;
        }
    }

    if (tok.kind != T_IDENT)
        die("typedef needs a name");
    strncpy(aname, tok.text, sizeof(aname) - 1);
    aname[sizeof(aname) - 1] = 0;
    next();

    if (sid >= 0)
        talias_add(aname, is_ptr ? 4 : structs[sid].size, sid, is_ptr);
    else
        talias_add(aname, sz, -1, is_ptr);
    expect_punct(";");
}

/* switch (e) { case C: ... default: ... }
 * 结构：  <e> -> eax; push eax; jmp dispatch; body...; jmp end;
 *         dispatch: mov ecx,[esp]; cmp ecx,Cn; je Ln; [jmp default];
 *         end: add esp,4 */
#define MAX_CASE 64

static void gen_switch(void)
{
    int cases[MAX_CASE], labels[MAX_CASE], nc = 0;
    int has_def = 0, def_label = 0;
    int disp, endj, endpos, i, d;

    next();                              /* switch */
    expect_punct("(");
    gen_expr();
    expect_punct(")");
    e8(0x50);                            /* push eax */
    disp = emit_jmp32();                 /* jmp dispatch */

    d = loop_depth;
    if (d >= MAX_LOOP)
        die("switch nesting too deep");
    loop_depth++;
    brk_n[d] = 0;

    expect_punct("{");
    while (!is_punct("}")) {
        if (is_word("case")) {
            int v;

            next();
            v = enum_val();
            if (nc >= MAX_CASE)
                die("too many case labels");
            cases[nc] = v;
            labels[nc] = code_len;
            nc++;
            expect_punct(":");
            continue;
        }
        if (is_word("default")) {
            next();
            expect_punct(":");
            has_def = 1;
            def_label = code_len;
            continue;
        }
        gen_stmt();
    }
    next();                              /* '}' */
    endj = emit_jmp32();                 /* 所有 case 执行完 -> end */

    patch_rel(disp, code_len);           /* dispatch 起点 */
    e8(0x8B); e8(0x0C); e8(0x24);        /* mov ecx,[esp] */
    for (i = 0; i < nc; i++) {
        int jp;

        e8(0x81); e8(0xF9); e32(cases[i]);   /* cmp ecx, imm32 */
        jp = je_placeholder(0x84);
        patch_rel(jp, labels[i]);
    }
    if (has_def) {
        int dj = emit_jmp32();

        patch_rel(dj, def_label);
    }
    endpos = code_len;
    patch_rel(endj, endpos);
    e8(0x83); e8(0xC4); e8(0x04);        /* add esp,4 */
    for (i = 0; i < brk_n[d]; i++)
        patch_rel(brk_pos[d][i], endpos);
    loop_depth--;
}


static void gen_block(void);
static void gen_var_decl(void);

static void gen_print(void)
{
    next();                              /* print */
    expect_punct("(");
    if (tok.kind == T_STR) {
        int id = add_string(tok.str, tok.slen);
        int len = tok.slen;
        next();
        expect_punct(")");
        expect_punct(";");
        e8(0xB9);                        /* mov ecx,imm32 */
        {
            int pos = e32(0);
            if (str_patch_n >= MAX_PATCH)
                die("too many string references");
            str_patch[str_patch_n] = pos;
            str_patch_id[str_patch_n] = id;
            str_patch_n++;
        }
        e8(0xBA); e32(len);              /* mov edx,len */
        e8(0xB8); e32(1);                /* eax=1 SYS_WRITE */
        e8(0xBB); e32(1);                /* ebx=1 stdout */
        e8(0xCD); e8(0x80);
        return;
    }

    /* 非字面量（字符串变量）：运行时按 NUL 结尾数字符长度 */
    gen_expr();
    expect_punct(")");
    expect_punct(";");
    e8(0x89); e8(0xC1);                  /* mov ecx,eax   (缓冲区) */
    e8(0x89); e8(0xC6);                  /* mov esi,eax   (游标) */
    e8(0x31); e8(0xD2);                  /* xor edx,edx   (长度 0) */
    {
        int loop = code_len;
        int jz, jmp;

        e8(0x80); e8(0x3C); e8(0x16); e8(0x00);   /* cmp byte [esi+edx],0 */
        jz = je_placeholder(0x84);
        e8(0x42);                                 /* inc edx */
        jmp = emit_jmp32();
        patch_rel(jmp, loop);
        patch_rel(jz, code_len);
    }
    e8(0xB8); e32(1);
    e8(0xBB); e32(1);
    e8(0xCD); e8(0x80);
}

static void gen_assign(const char *name)
{
    var_t *vp = var_lookup(name);
    if (!vp)
        die("assignment to an undefined variable");

    /* 复合赋值：x op= expr（只对简单变量，数组/成员暂不支持） */
    if (is_punct("+=") || is_punct("-=") || is_punct("*=") ||
        is_punct("/=") || is_punct("%=") || is_punct("|=") ||
        is_punct("&=") || is_punct("^=") || is_punct("<<=") ||
        is_punct(">>=")) {
        char op = tok.text[0];
        int is_shift = (tok.text[2] != 0);

        if (vp->is_array)
            die("compound assignment to an array");
        next();
        emit_load(vp);                       /* eax = 旧值 */
        e8(0x50);                            /* push 旧值 */
        gen_expr();                          /* eax = 右值 */
        e8(0x89); e8(0xC1);                  /* mov ecx,eax */
        e8(0x58);                            /* pop eax */
        /* 指针 += / -= 时右值按元素宽度缩放 */
        if (op == '+' || op == '-') {
            int stride = 0;
            if (vp->ptr_elem > 0)
                stride = vp->ptr_elem;
            else if (vp->is_struct_ptr && vp->struct_id >= 0)
                stride = structs[vp->struct_id].size;
            if (stride == 2)      { e8(0xC1); e8(0xE1); e8(0x01); }
            else if (stride == 4) { e8(0xC1); e8(0xE1); e8(0x02); }
            else if (stride == 8) { e8(0xC1); e8(0xE1); e8(0x03); }
            else if (stride > 0)  { e8(0x69); e8(0xC9); e32(stride); }
        }
        if (op == '+') {
            e8(0x01); e8(0xC8);              /* add eax,ecx */
        } else if (op == '-') {
            e8(0x29); e8(0xC8);              /* sub eax,ecx */
        } else if (op == '*') {
            e8(0x0F); e8(0xAF); e8(0xC1);    /* imul eax,ecx */
        } else if (op == '/') {
            e8(0x99);                        /* cdq */
            e8(0xF7); e8(0xF9);              /* idiv ecx */
        } else if (op == '%') {
            e8(0x99);                        /* cdq */
            e8(0xF7); e8(0xF9);              /* idiv ecx */
            e8(0x89); e8(0xD0);              /* mov eax,edx */
        } else if (is_shift) {
            if (op == '<') { e8(0xD3); e8(0xE0); }   /* shl eax,cl */
            else           { e8(0xD3); e8(0xF8); }   /* sar eax,cl */
        } else if (op == '|') {
            e8(0x09); e8(0xC8);              /* or  eax,ecx */
        } else if (op == '&') {
            e8(0x21); e8(0xC8);              /* and eax,ecx */
        } else {
            e8(0x31); e8(0xC8);              /* xor eax,ecx */
        }
        emit_store(vp);
        return;
    }

    if (is_punct(".") || is_punct("->")) {
        gen_member_addr(vp);
        e8(0x52);                            /* push edx（先保住地址） */
        expect_punct("=");
        gen_expr();
        e8(0x59);                            /* pop ecx */
        e8(0x89); e8(0x01);                  /* mov [ecx],eax */
        return;
    }

    if (is_punct("[")) {
        int stride = 4;
        if (vp->struct_id >= 0 && vp->is_array)
            stride = structs[vp->struct_id].size;
        else if (vp->ptr_elem > 0)
            stride = vp->ptr_elem;
        else if (vp->is_array && vp->elem_size > 0)
            stride = vp->elem_size;
        next();
        gen_expr();                              /* eax = index */
        expect_punct("]");
        if (stride == 2)      { e8(0xC1); e8(0xE0); e8(0x01); }
        else if (stride == 4) { e8(0xC1); e8(0xE0); e8(0x02); }
        else if (stride == 8) { e8(0xC1); e8(0xE0); e8(0x03); }
        else                  { e8(0x69); e8(0xC0); e32(stride); }
        e8(0x50);                                /* push 下标*stride */
        if (vp->struct_id >= 0 && vp->is_array && is_punct(".")) {
            struct_t *s = &structs[vp->struct_id];
            int idx = field_take(s);
            expect_punct("=");
            gen_expr();                          /* eax = value */
            e8(0x5A);                            /* pop edx */
            if (vp->is_global) { e8(0x8D); e8(0x0D); e32(CODE_BASE + GC_BASE + vp->gaddr); }
            else               { e8(0x8D); e8(0x8D); e32(vp->off); }
            e8(0x01); e8(0xD1);                  /* add ecx,edx */
            e8(0x89); e8(0x81); e32(s->foff[idx]);   /* mov [ecx+foff],eax */
            return;
        }
        expect_punct("=");
        gen_expr();                              /* eax = value */
        e8(0x5A);                                /* pop edx */
        if (vp->is_array) {
            if (vp->is_global) { e8(0x8D); e8(0x0D); e32(CODE_BASE + GC_BASE + vp->gaddr); }
            else               { e8(0x8D); e8(0x8D); e32(vp->off); }
        } else {
            /* 标量当指针：ecx = 指针值 */
            if (vp->is_global) { e8(0x8B); e8(0x0D); e32(CODE_BASE + GC_BASE + vp->gaddr); }
            else               { e8(0x8B); e8(0x8D); e32(vp->off); }
        }
        e8(0x01); e8(0xD1);                      /* add ecx,edx */
        if (stride == 1)      { e8(0x88); e8(0x01); }             /* mov [ecx],al  */
        else if (stride == 2) { e8(0x66); e8(0x89); e8(0x01); }   /* mov [ecx],ax  */
        else                  { e8(0x89); e8(0x01); }             /* mov [ecx],eax */
        return;
    }

    expect_punct("=");
    gen_expr();
    emit_store(vp);
}

static void gen_if(void)
{
    int jz_pos, jmp_pos = -1;

    next();
    expect_punct("(");
    gen_expr();
    expect_punct(")");
    e8(0x85); e8(0xC0);
    jz_pos = je_placeholder(0x84);

    gen_stmt();

    if (is_word("else")) {
        next();
        jmp_pos = emit_jmp32();
        patch_rel(jz_pos, code_len);
        gen_stmt();
        patch_rel(jmp_pos, code_len);
    } else {
        patch_rel(jz_pos, code_len);
    }
}

static void gen_while(void)
{
    int start, jz_pos, back, d, k;

    if (loop_depth >= MAX_LOOP)
        die("loops nested too deeply");
    d = loop_depth;
    cont_n[d] = 0;
    brk_n[d] = 0;
    start = code_len;
    loop_depth++;

    next();
    expect_punct("(");
    gen_expr();
    expect_punct(")");
    e8(0x85); e8(0xC0);
    jz_pos = je_placeholder(0x84);

    gen_stmt();

    back = emit_jmp32();
    patch_rel(back, start);
    patch_rel(jz_pos, code_len);

    for (k = 0; k < cont_n[d]; k++)
        patch_rel(cont_pos[d][k], start);       /* continue 回到条件判断 */
    for (k = 0; k < brk_n[d]; k++)
        patch_rel(brk_pos[d][k], code_len);     /* break 跳到循环之后 */
    loop_depth--;
}

/* for 的初始化部分：支持声明和赋值两种写法，自带分号 */
static void gen_for_init(void)
{
    if (tok.kind == T_IDENT && talias_id_of(tok.text) >= 0) {
        gen_var_decl();
        return;
    }
    if (is_word("int") || is_word("char") || is_word("short") ||
        is_word("long") || is_word("unsigned") || is_word("signed") ||
        is_word("static") || is_word("const") || is_word("volatile")) {
        gen_var_decl();
        return;
    }
    if (tok.kind == T_IDENT) {
        char name[48];
        strncpy(name, tok.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();
        if (is_punct("=")) {
            gen_assign(name);
            expect_punct(";");
            return;
        }
        die("bad for-init");
    }
    gen_expr();
    expect_punct(";");
}

/* for 的步进部分：不带分号，以 ')' 结束 */
static void gen_for_step(void)
{
    if (tok.kind == T_IDENT) {
        char name[48];
        var_t *vp;
        int step;

        strncpy(name, tok.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();
        if (is_punct("++") || is_punct("--")) {
            vp = var_lookup(name);
            if (!vp)
                die("step of an undefined variable");
            if (vp->is_array)
                die("cannot step an array");
            step = (vp->ptr_elem > 0) ? vp->ptr_elem : 1;
            if (tok.text[0] == '-')
                step = -step;
            next();
            if (vp->is_global) {
                e8(0x81); e8(0x05); e32(CODE_BASE + GC_BASE + vp->gaddr); e32(step);
            } else {
                e8(0x81); e8(0x85); e32(vp->off); e32(step);
            }
            return;
        }
        if (is_punct("=") || is_punct("|=") || is_punct("&=") ||
            is_punct("^=") || is_punct("<<=") || is_punct(">>=")) {
            gen_assign(name);
            return;
        }
        die("bad for-step");
    }
    gen_expr();
}

/* for (init; cond; step) stmt
 *
 * 源码里 step 写在 body 前面，但机器码里 step 必须排在 body 之后，
 * 所以先把 step 生成到临时缓冲暂存，等 body 编完再把字节接回去。
 */
static void gen_for(void)
{
    static unsigned char step_buf[MAX_LOOP][512];   /* 按嵌套深度分层，避免内层覆盖外层 */
    int top, jz_pos, back, step_pos, step_start, step_len, d, k;

    if (loop_depth >= MAX_LOOP)
        die("loops nested too deeply");
    d = loop_depth;
    cont_n[d] = 0;
    brk_n[d] = 0;

    next();                                  /* for */
    expect_punct("(");
    if (is_punct(";"))
        next();
    else
        gen_for_init();

    top = code_len;
    loop_depth++;

    if (is_punct(";")) {
        jz_pos = -1;                         /* 无条件 = 永真 */
        next();
    } else {
        gen_expr();
        e8(0x85); e8(0xC0);
        jz_pos = je_placeholder(0x84);
        expect_punct(";");
    }

    /* 先按源码顺序把 step 解析并生成出来，暂存字节后回退代码指针 */
    step_start = code_len;
    if (is_punct(")")) {
        next();                              /* 没有 step */
        step_len = 0;
    } else {
        gen_for_step();
        expect_punct(")");
        step_len = code_len - step_start;
        if (step_len > 512)
            die("for-step too large");
        {
            int i;
            for (i = 0; i < step_len; i++)
                step_buf[d][i] = code[step_start + i];
        }
        code_len = step_start;               /* 回退，body 从这里开始 */
    }

    gen_stmt();                              /* 循环体 */

    step_pos = code_len;                     /* continue 的目标：body 之后 */
    if (step_len > 0) {
        int i;
        for (i = 0; i < step_len; i++)
            code[code_len++] = step_buf[d][i];
    }

    back = emit_jmp32();
    patch_rel(back, top);
    if (jz_pos >= 0)
        patch_rel(jz_pos, code_len);

    for (k = 0; k < cont_n[d]; k++)
        patch_rel(cont_pos[d][k], step_pos);    /* continue 跳到步进 */
    for (k = 0; k < brk_n[d]; k++)
        patch_rel(brk_pos[d][k], code_len);
    loop_depth--;
}

static void gen_return(void)
{
    next();
    if (is_punct(";")) {
        e8(0x31); e8(0xC0);              /* xor eax,eax */
    } else {
        gen_expr();
    }
    expect_punct(";");
    e8(0x89); e8(0xEC);                  /* mov esp,ebp */
    e8(0x5D);                            /* pop ebp */
    e8(0xC3);                            /* ret */
}

static void gen_var_decl(void)
{
    char name[48];
    int count = 0;
    int v;
    int tsz;

    tsz = skip_type_prefix();
    {
        int pelem = 0;
        int decl_sid = last_type_struct;

        if (is_punct("*")) {                 /* char *p / int *p */
            next();
            pelem = tsz;
            tsz = 4;
        }
        var_decl_ptr_elem = pelem;
        var_decl_struct_id = decl_sid;
        var_decl_is_struct_ptr = (pelem != 0 && decl_sid >= 0);
    }
    if (tok.kind != T_IDENT)
        die("expected a variable name");
    strncpy(name, tok.text, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    next();

    if (is_punct("[")) {
        next();
        if (tok.kind != T_NUM)
            die("array size must be a constant");
        count = tok.num;
        if (count <= 0 || count > 2048)
            die("bad array size");
        next();
        expect_punct("]");
        var_add(name, count * tsz);
        var_mark_array(nvars - 1, count);
        vars[nvars - 1].elem_size = tsz;
        vars[nvars - 1].is_unsigned = last_type_unsigned;
        if (var_decl_struct_id >= 0) {
            vars[nvars - 1].struct_id = var_decl_struct_id;
            vars[nvars - 1].is_struct_ptr = 0;
        }
        expect_punct(";");
        var_decl_ptr_elem = 0;
        return;
    }

    v = var_add(name, (tsz < 4) ? 4 : tsz);
    vars[nvars - 1].ptr_elem = var_decl_ptr_elem;
    vars[nvars - 1].is_unsigned = last_type_unsigned;
    if (var_decl_struct_id >= 0) {
        vars[nvars - 1].struct_id = var_decl_struct_id;
        vars[nvars - 1].is_struct_ptr = var_decl_is_struct_ptr;
    }
    var_decl_ptr_elem = 0;
    var_decl_struct_id = -1;
    var_decl_is_struct_ptr = 0;
    if (is_punct("=")) {
        next();
        gen_expr();
        e8(0x89); e8(0x85); e32(v);
    }
    expect_punct(";");
}

/* struct P p;  struct P *q;  局部声明 */
static void gen_struct_decl_local(void)
{
    char sname[48], name[48];
    struct_t *s;
    int sid, is_ptr = 0, v, size;

    next();                              /* struct */
    if (tok.kind != T_IDENT)
        die("expected a struct name");
    strncpy(sname, tok.text, sizeof(sname) - 1);
    sname[sizeof(sname) - 1] = 0;
    next();
    sid = struct_id_of(sname);
    if (sid < 0)
        die("unknown struct type");
    s = &structs[sid];
    if (is_punct("*")) { next(); is_ptr = 1; }

    if (tok.kind != T_IDENT)
        die("expected a variable name");
    strncpy(name, tok.text, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    next();

    if (is_punct("[") && !is_ptr) {
        int count;
        next();
        if (tok.kind != T_NUM)
            die("array size must be a constant");
        count = tok.num;
        if (count <= 0 || count > 256)
            die("bad array size");
        next();
        expect_punct("]");
        var_add(name, count * s->size);
        vars[nvars - 1].struct_id = sid;
        vars[nvars - 1].is_struct_ptr = 0;
        var_mark_array(nvars - 1, count);
        expect_punct(";");
        return;
    }

    size = is_ptr ? 4 : s->size;
    v = var_add(name, size);
    vars[nvars - 1].struct_id = sid;
    vars[nvars - 1].is_struct_ptr = is_ptr;

    if (is_punct("=")) {
        next();
        gen_expr();
        e8(0x89); e8(0x85); e32(v);
    }
    expect_punct(";");
}

static void gen_stmt(void)
{
    if (is_punct("{")) {
        gen_block();
        return;
    }
    if (is_punct("*")) {                 /* *p = value; */
        next();
        gen_unary();                     /* eax = 目标地址 */
        e8(0x50);                        /* push 地址 */
        expect_punct("=");
        gen_expr();                      /* eax = 值 */
        e8(0x5A);                        /* pop edx */
        e8(0x89); e8(0x02);              /* mov [edx],eax */
        expect_punct(";");
        return;
    }
    if (is_word("int") || is_word("char") || is_word("short") ||
        is_word("long") || is_word("unsigned") || is_word("signed") ||
        is_word("static") || is_word("const") || is_word("volatile")) {
        gen_var_decl();
        return;
    }
    if (is_word("struct")) {
        gen_struct_decl_local();
        return;
    }
    if (is_word("typedef")) {
        gen_typedef();
        return;
    }
    if (is_word("enum")) {
        gen_enum();
        return;
    }
    if (is_word("switch")) {
        gen_switch();
        return;
    }
    if (tok.kind == T_IDENT && talias_id_of(tok.text) >= 0) {
        gen_var_decl();
        return;
    }
    if (is_word("if")) {
        gen_if();
        return;
    }
    if (is_word("while")) {
        gen_while();
        return;
    }
    if (is_word("for")) {
        gen_for();
        return;
    }
    if (is_word("break")) {
        int d = loop_depth - 1;
        if (d < 0)
            die("break outside of a loop");
        if (brk_n[d] >= MAX_LOOP_JMP)
            die("too many breaks in one loop");
        next();
        expect_punct(";");
        brk_pos[d][brk_n[d]++] = emit_jmp32();
        return;
    }
    if (is_word("continue")) {
        int d = loop_depth - 1;
        if (d < 0)
            die("continue outside of a loop");
        if (cont_n[d] >= MAX_LOOP_JMP)
            die("too many continues in one loop");
        next();
        expect_punct(";");
        cont_pos[d][cont_n[d]++] = emit_jmp32();
        return;
    }
    if (is_word("return")) {
        gen_return();
        return;
    }
    if (is_word("print")) {
        gen_print();
        return;
    }
    if (tok.kind == T_IDENT) {
        char name[48];
        strncpy(name, tok.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();
        if (is_punct("=") || is_punct("[") || is_punct(".") || is_punct("->") ||
            is_punct("+=") || is_punct("-=") || is_punct("*=") || is_punct("/=") ||
            is_punct("%=") ||
            is_punct("|=") || is_punct("&=") || is_punct("^=") ||
            is_punct("<<=") || is_punct(">>=")) {
            gen_assign(name);
            expect_punct(";");
            return;
        }
        if (is_punct("(")) {
            gen_call(name);
            expect_punct(";");
            return;
        }
        if (is_punct("++") || is_punct("--")) {
            var_t *vp = var_lookup(name);
            int step;

            if (!vp)
                die("increment of an undefined variable");
            if (vp->is_array)
                die("cannot increment an array");
            step = (vp->ptr_elem > 0) ? vp->ptr_elem : 1;
            if (tok.text[0] == '-')
                step = -step;
            next();
            if (vp->is_global) {
                e8(0x81); e8(0x05); e32(CODE_BASE + GC_BASE + vp->gaddr); e32(step);
            } else {
                e8(0x81); e8(0x85); e32(vp->off); e32(step);
            }
            expect_punct(";");
            return;
        }
        die("bad statement");
    }
    gen_expr();
    expect_punct(";");
}

static void gen_block(void)
{
    expect_punct("{");
    while (!is_punct("}"))
        gen_stmt();
    expect_punct("}");
}

/* ---------------- 函数 ---------------- */
static void gen_func_body(const char *name)
{
    char pnames[6][48];
    int  p_sid[6], p_ptr[6], p_pelem[6];
    int f, nparams = 0;

    expect_punct("(");
    if (!is_punct(")")) {
        for (;;) {
            int sid = -1, is_ptr = 0;

            if (is_word("struct")) {
                char sname[48];
                next();
                if (tok.kind != T_IDENT)
                    die("expected a struct name");
                strncpy(sname, tok.text, sizeof(sname) - 1);
                sname[sizeof(sname) - 1] = 0;
                next();
                sid = struct_id_of(sname);
                if (sid < 0)
                    die("unknown struct type");
                if (!is_punct("*"))
                    die("only 'struct P *' parameters are supported");
                next();
                is_ptr = 1;
                p_pelem[nparams] = 0;
            } else {
                int pt = skip_type_prefix();
                if (is_punct(")"))
                    break;                  /* 形如 f(void)：空参数表 */
                if (is_punct("*")) {         /* char *p 形参 */
                    next();
                    p_pelem[nparams] = pt;
                } else {
                    p_pelem[nparams] = 0;
                }
                if (tok.kind != T_IDENT)
                    die("expected a parameter name");
                if (is_punct("*"))
                    die("only 'char *' pointers are not supported yet");
            }
            if (tok.kind != T_IDENT)
                die("expected a parameter name");
            strncpy(pnames[nparams], tok.text, sizeof(pnames[0]) - 1);
            pnames[nparams][sizeof(pnames[0]) - 1] = 0;
            next();
            p_sid[nparams] = sid;
            p_ptr[nparams] = is_ptr;
            nparams++;
            if (nparams > 6)
                die("at most 6 parameters");
            if (is_punct(",")) { next(); continue; }
            break;
        }
    }
    expect_punct(")");

    f = func_add(name, nparams);
    funcs[f].off = code_len;

    e8(0x55);                            /* push ebp */
    e8(0x89); e8(0xE5);                  /* mov ebp,esp */
    e8(0x81); e8(0xEC);                  /* sub esp,imm32 */
    prologue_pos = e32(0);

    nvars = 0;
    locals = 0;
    /* 调用方是从左到右压栈的，所以源码里的第 i 个参数落在栈上第 (n-1-i) 个位置 */
    for (int i = 0; i < nparams; i++) {
        var_add_param(pnames[i], 8 + 4 * (nparams - 1 - i));
        vars[nvars - 1].struct_id = p_sid[i];
        vars[nvars - 1].is_struct_ptr = p_ptr[i];
        vars[nvars - 1].ptr_elem = p_pelem[i];
    }

    gen_block();

    e8(0x89); e8(0xEC);                  /* mov esp,ebp */
    e8(0x5D);
    e8(0xC3);

    patch32(prologue_pos, (locals + 15) & ~15);
}

static void gen_global_decl(const char *name, int tsz)
{
    int count;
    int pelem = var_decl_ptr_elem;

    var_decl_ptr_elem = 0;

    if (is_punct("[")) {
        next();
        if (tok.kind != T_NUM)
            die("array size must be a constant");
        count = tok.num;
        if (count <= 0 || count > 2048)
            die("bad array size");
        next();
        expect_punct("]");
        global_add(name, count * tsz, 1, count);
        gvars[ngvars - 1].elem_size = tsz;
        if (var_decl_struct_id >= 0) {
            gvars[ngvars - 1].struct_id = var_decl_struct_id;
            gvars[ngvars - 1].is_struct_ptr = 0;
        }
        if (is_punct("=")) {                     /* int a[3] = { 1, 2, 3 }; */
            int k = 0;

            next();
            if (!is_punct("{"))
                die("array initializer must be { ... }");
            next();
            while (k < count && !is_punct("}")) {
                if (nginit >= MAX_GINIT)
                    die("too many global initializers");
                ginit[nginit].addr = gvars[ngvars - 1].gaddr + k * tsz;
                ginit[nginit].size = (tsz < 4) ? tsz : 4;
                ginit[nginit].val  = const_expr_simple();
                nginit++;
                k++;
                if (is_punct(","))
                    next();
                else
                    break;
            }
            expect_punct("}");
            if (k < count)
                printf("tc: note: %s initialized %d/%d elements\n", name, k, count);
        }
    } else {
        global_add(name, (tsz < 4) ? 4 : tsz, 0, 1);
        gvars[ngvars - 1].elem_size = (tsz < 4) ? tsz : 4;
        gvars[ngvars - 1].ptr_elem = pelem;
        if (var_decl_struct_id >= 0) {
            gvars[ngvars - 1].struct_id = var_decl_struct_id;
            gvars[ngvars - 1].is_struct_ptr = (pelem != 0);
        }
    }
    var_decl_struct_id = -1;
    var_decl_is_struct_ptr = 0;

    if (is_punct("=")) {
        int sz;

        next();
        sz = gvars[ngvars - 1].elem_size;
        if (sz != 1 && sz != 2)
            sz = 4;
        if (nginit < MAX_GINIT) {
            ginit[nginit].addr = gvars[ngvars - 1].gaddr;
            ginit[nginit].size = sz;
            ginit[nginit].val  = const_expr_simple();
            nginit++;
        } else {
            die("too many global initializers");
        }
    }
    expect_punct(";");
}

/* struct P { ... };  定义；或 struct P x;  全局变量 */
static void gen_struct_top(void)
{
    char sname[24];
    struct_t *s;
    int size = 0;

    next();                              /* struct */
    if (tok.kind != T_IDENT)
        die("expected a struct name");
    strncpy(sname, tok.text, sizeof(sname) - 1);
    sname[sizeof(sname) - 1] = 0;
    next();

    if (!is_punct("{")) {
        char gname[48];
        int is_ptr = 0, sid;

        sid = struct_id_of(sname);
        if (sid < 0)
            die("unknown struct type");
        s = &structs[sid];
        if (is_punct("*")) { next(); is_ptr = 1; }
        if (tok.kind != T_IDENT)
            die("expected a global name");
        strncpy(gname, tok.text, sizeof(gname) - 1);
        gname[sizeof(gname) - 1] = 0;
        next();
        if (is_punct("[") && !is_ptr) {
            int count;
            next();
            if (tok.kind != T_NUM)
                die("array size must be a constant");
            count = tok.num;
            if (count <= 0 || count > 256)
                die("bad array size");
            next();
            expect_punct("]");
            global_add(gname, count * s->size, 1, count);
            gvars[ngvars - 1].struct_id = sid;
            gvars[ngvars - 1].is_struct_ptr = 0;
            if (is_punct("="))
                die("global initializers are not supported (assign them in main())");
            expect_punct(";");
            return;
        }
        global_add(gname, is_ptr ? 4 : s->size, 0, 1);
        gvars[ngvars - 1].struct_id = sid;
        gvars[ngvars - 1].is_struct_ptr = is_ptr;
        if (is_punct("="))
            die("global initializers are not supported (assign them in main())");
        expect_punct(";");
        return;
    }

    (void)size;
    struct_def_body(sname);
    expect_punct(";");
}

static void compile_unit(void)
{
    while (tok.kind != T_EOF) {
        char name[48];
        int tsz;

        if (is_word("typedef")) {
            gen_typedef();
            continue;
        }
        if (is_word("enum")) {
            gen_enum();
            continue;
        }
        if (is_word("struct")) {
            gen_struct_top();
            continue;
        }
        tsz = skip_type_prefix();
        if (is_punct("*")) {                 /* int *g; 全局指针 */
            next();
            var_decl_ptr_elem = tsz;
            tsz = 4;
        }
        if (tok.kind != T_IDENT)
            die("expected a name after the type");
        strncpy(name, tok.text, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();

        if (is_punct("("))
            gen_func_body(name);
        else
            gen_global_decl(name, tsz);
    }
}

/* ---------------- ELF 输出 ---------------- */
static int build_elf(unsigned char *out, int cap)
{
    int total = HDR_SIZE + code_len + strs_len;
    unsigned int *u;
    unsigned short *s;

    if (total > cap)
        die("output too large");

    memset(out, 0, (unsigned)HDR_SIZE);
    out[0] = 0x7F; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
    out[4] = 1; out[5] = 1; out[6] = 1;

    s = (unsigned short *)(out + 16);
    s[0] = 2;                            /* e_type ET_EXEC */
    s[1] = 3;                            /* e_machine EM_386 */
    s[2] = 1;                            /* e_version */

    u = (unsigned int *)(out + 20);
    u[0] = 0;                            /* e_flags */

    u = (unsigned int *)(out + 24);
    u[0] = CODE_BASE + HDR_SIZE;         /* e_entry */
    u[1] = 52;                           /* e_phoff */
    u[2] = 0;                            /* e_shoff */
    u[3] = 0;

    s = (unsigned short *)(out + 40);
    s[0] = 52;                           /* e_ehsize */
    s[1] = 32;                           /* e_phentsize */
    s[2] = 1;                            /* e_phnum */
    s[3] = 0;
    s[4] = 0;
    s[5] = 0;

    u = (unsigned int *)(out + 52);
    u[0] = 1;                            /* p_type PT_LOAD */
    u[1] = 0;                            /* p_offset */
    u[2] = CODE_BASE;                    /* p_vaddr */
    u[3] = CODE_BASE;                    /* p_paddr */
    u[4] = (unsigned)total;              /* p_filesz */
    {
        unsigned memsz = (unsigned)total;
        int need = GC_BASE + gdata_len;   /* 全局 BSS 必须被 memsz 覆盖 */
        if (need > (int)memsz)
            memsz = (unsigned)need;
        u[5] = memsz;                    /* p_memsz */
    }
    u[6] = 7;                            /* p_flags RWX */
    u[7] = 0x1000;                       /* p_align */

    memcpy(out + HDR_SIZE, code, (unsigned)code_len);
    if (strs_len)
        memcpy(out + HDR_SIZE + code_len, strs, (unsigned)strs_len);
    return total;
}

/* ---------------- 入口 ---------------- */
static unsigned char src_buf[SRC_MAX * 2];
static unsigned char exp_buf[SRC_MAX * 2];
static unsigned char inc_buf0[SRC_MAX];
static unsigned char inc_buf1[SRC_MAX];
static unsigned char elf_buf[CODE_MAX + STR_MAX + 256];

/* 读一个文件到缓冲，返回字节数，失败 -1 */
static int load_file(const char *path, unsigned char *buf, int cap)
{
    int fd = sys_open(path, 0), n = 0, r;

    if (fd < 0)
        return -1;
    for (;;) {
        r = sys_read(fd, buf + n, cap - n);
        if (r <= 0)
            break;
        n += r;
        if (n >= cap)
            break;
    }
    sys_close(fd);
    return n;
}

/* 把行首的 #include "x.h" / #include <x.h> 换成 /DOCS/x.h 的内容。
   只展开一层：头文件里再写 #include 就原样保留（不会报错，但也不会展开）。 */
/* ---------------- 函数式宏（#define NAME(a,b) body） ---------------- */
#define MAX_FMAC   16
#define FMAC_PARAM 80
#define FMAC_BODY  180

typedef struct {
    char name[32];
    char params[FMAC_PARAM];   /* 形参名平铺存放，按 20 字节一格 */
    int  nparams;
    char body[FMAC_BODY];
} fmac_t;

static fmac_t fmacs[MAX_FMAC];
static int    nfmacs;

static int fmac_find(const char *n)
{
    int i;

    for (i = 0; i < nfmacs; i++)
        if (strcmp(fmacs[i].name, n) == 0)
            return i;
    return -1;
}

/* 逐行扫描，登记“宏名紧跟左括号”的定义；行首 #define 本身保留给后续流程 */
static void fmac_collect(const unsigned char *in, int len)
{
    int i = 0;

    nfmacs = 0;
    while (i < len) {
        int ls = i, p, q;

        while (i < len && in[i] != '\n')
            i++;
        p = ls;
        while (p < i && (in[p] == ' ' || in[p] == '\t'))
            p++;
        if (p < i && in[p] == '#') {
            q = p + 1;
            while (q < i && (in[q] == ' ' || in[q] == '\t'))
                q++;
            if (q + 6 <= i && in[q] == 'd' && in[q + 1] == 'e' && in[q + 2] == 'f' &&
                in[q + 3] == 'i' && in[q + 4] == 'n' && in[q + 5] == 'e') {
                int nm, ne;
                char nmbuf[32];

                q += 6;
                while (q < i && (in[q] == ' ' || in[q] == '\t'))
                    q++;
                nm = q;
                while (q < i && is_id(in[q]))
                    q++;
                ne = q;
                if (q < i && in[q] == '(' && nfmacs < MAX_FMAC && ne > nm && ne - nm < 31) {
                    int k = 0, np = 0;

                    while (nm < ne)
                        nmbuf[k++] = (char)in[nm++];
                    nmbuf[k] = 0;

                    q++;                              /* 跳过 '(' */
                    for (;;) {
                        char pn[20];
                        int pk = 0;

                        while (q < i && (in[q] == ' ' || in[q] == '\t'))
                            q++;
                        while (q < i && is_id(in[q]) && pk < 19)
                            pn[pk++] = (char)in[q++];
                        pn[pk] = 0;
                        if (pk > 0 && np < 4) {
                            int t;

                            for (t = 0; t < pk; t++)
                                fmacs[nfmacs].params[np * 20 + t] = pn[t];
                            fmacs[nfmacs].params[np * 20 + pk] = 0;
                            np++;
                        }
                        while (q < i && (in[q] == ' ' || in[q] == '\t'))
                            q++;
                        if (q < i && in[q] == ',') { q++; continue; }
                        if (q < i && in[q] == ')') { q++; break; }
                        break;
                    }
                    fmacs[nfmacs].nparams = np;
                    strncpy(fmacs[nfmacs].name, nmbuf, 31);
                    fmacs[nfmacs].name[31] = 0;

                    k = 0;
                    while (q < i && (in[q] == ' ' || in[q] == '\t'))
                        q++;
                    while (q < i && in[q] != '\r' && k < FMAC_BODY - 1)
                        fmacs[nfmacs].body[k++] = (char)in[q++];
                    fmacs[nfmacs].body[k] = 0;
                    nfmacs++;
                }
            }
        }
        if (i < len)
            i++;
    }
}

/* 把 NAME(...) 原地换成宏体；跳过字符串和行注释，实参支持嵌套括号 */
static int expand_func_macros(const unsigned char *in, int inlen,
                              unsigned char *out, int cap)
{
    int i = 0, o = 0, bol = 1;

    while (i < inlen) {
        /* 预处理指令行（#define / #ifdef ...）整行原样拷贝，绝不展开 */
        if (bol) {
            int p = i;

            while (p < inlen && (in[p] == ' ' || in[p] == '\t'))
                p++;
            if (p < inlen && in[p] == '#') {
                while (i < inlen && in[i] != '\n')
                    out[o++] = in[i++];
                if (i < inlen) {
                    out[o++] = in[i++];
                    bol = 1;
                }
                continue;
            }
            bol = 0;
        }
        if (in[i] == '\n') {
            out[o++] = in[i++];
            bol = 1;
            continue;
        }
        if (in[i] == '"') {                            /* 字符串原样拷贝 */
            out[o++] = in[i++];
            while (i < inlen && in[i] != '"') {
                if (in[i] == '\\' && i + 1 < inlen)
                    out[o++] = in[i++];
                out[o++] = in[i++];
            }
            if (i < inlen)
                out[o++] = in[i++];
            continue;
        }
        if (in[i] == '/' && i + 1 < inlen && in[i + 1] == '/') {
            while (i < inlen && in[i] != '\n')
                out[o++] = in[i++];
            continue;
        }
        if (is_id0(in[i])) {
            char w[40];
            int k = 0, st = i, fi;

            while (i < inlen && is_id(in[i]) && k < 39)
                w[k++] = (char)in[i++];
            w[k] = 0;
            fi = fmac_find(w);
            if (fi >= 0) {
                int j = i, nargs = 0;
                char args[4][80];
                int  alen[4];
                int ok = 0;

                while (j < inlen && (in[j] == ' ' || in[j] == '\t'))
                    j++;
                if (j < inlen && in[j] == '(') {
                    j++;
                    alen[0] = 0;
                    for (;;) {
                        int d = 0;

                        for (;;) {
                            if (j >= inlen)
                                break;
                            if (in[j] == '"') {
                                if (alen[nargs] < 78) args[nargs][alen[nargs]++] = (char)in[j];
                                j++;
                                while (j < inlen && in[j] != '"') {
                                    if (alen[nargs] < 78) args[nargs][alen[nargs]++] = (char)in[j];
                                    j++;
                                }
                                if (j < inlen) {
                                    if (alen[nargs] < 78) args[nargs][alen[nargs]++] = (char)in[j];
                                    j++;
                                }
                                continue;
                            }
                            if (in[j] == '(') { d++; }
                            else if (in[j] == ')') {
                                if (d == 0) break;
                                d--;
                            } else if (in[j] == ',' && d == 0) {
                                break;
                            }
                            if (alen[nargs] < 78)
                                args[nargs][alen[nargs]++] = (char)in[j];
                            j++;
                        }
                        args[nargs][alen[nargs]] = 0;
                        while (alen[nargs] > 0 &&
                               (args[nargs][alen[nargs] - 1] == ' ' ||
                                args[nargs][alen[nargs] - 1] == '\t'))
                            args[nargs][--alen[nargs]] = 0;
                        /* 去掉参数头部空白 */
                        {
                            int h = 0, t = 0;
                            char tmp[80];

                            while (h < alen[nargs] &&
                                   (args[nargs][h] == ' ' || args[nargs][h] == '\t'))
                                h++;
                            while (h < alen[nargs])
                                tmp[t++] = args[nargs][h++];
                            tmp[t] = 0;
                            alen[nargs] = t;
                            for (h = 0; h < t; h++)
                                args[nargs][h] = tmp[h];
                        }
                        nargs++;
                        if (nargs >= 4)
                            break;
                        if (j < inlen && in[j] == ',') {
                            j++;
                            alen[nargs] = 0;
                            continue;
                        }
                        break;
                    }
                    if (j < inlen && in[j] == ')')
                        j++;
                    ok = 1;
                }

                if (ok) {
                    int b = 0;

                    while (fmacs[fi].body[b] && o < cap - 2) {
                        if (is_id0(fmacs[fi].body[b])) {
                            char bw[20];
                            int bk = 0, bs = b, t, m = -1;

                            while (fmacs[fi].body[b] && is_id(fmacs[fi].body[b]) && bk < 19)
                                bw[bk++] = fmacs[fi].body[b++];
                            bw[bk] = 0;
                            for (t = 0; t < fmacs[fi].nparams; t++) {
                                char *pn = &fmacs[fi].params[t * 20];

                                if (strcmp(bw, pn) == 0 && t < nargs)
                                    m = t;
                            }
                            if (m >= 0) {
                                int a2;

                                for (a2 = 0; a2 < alen[m] && o < cap - 2; a2++)
                                    out[o++] = args[m][a2];
                            } else {
                                int a3;

                                for (a3 = bs; a3 < b && o < cap - 2; a3++)
                                    out[o++] = fmacs[fi].body[a3];
                            }
                            continue;
                        }
                        out[o++] = fmacs[fi].body[b];
                        b++;
                    }
                    i = j;
                    continue;
                }
            }
            {
                int t;

                for (t = st; t < i && o < cap - 1; t++)
                    out[o++] = in[t];
            }
            continue;
        }
        if (o < cap - 1)
            out[o++] = in[i++];
    }
    out[o] = 0;
    return o;
}

static int expand_includes(const unsigned char *in, int inlen,
                           unsigned char *out, int cap, int depth)
{
    int i = 0, o = 0;

    while (i < inlen) {
        int p = i, q, w;

        while (p < inlen && (in[p] == ' ' || in[p] == '\t'))
            p++;
        if (p < inlen && in[p] == '#') {
            q = p + 1;
            while (q < inlen && (in[q] == ' ' || in[q] == '\t'))
                q++;
            w = q;
            while (q < inlen && is_id(in[q]))
                q++;
            if (q - w == 7 && in[w] == 'i' && in[w + 1] == 'n' && in[w + 2] == 'c' &&
                in[w + 3] == 'l' && in[w + 4] == 'u' && in[w + 5] == 'd' && in[w + 6] == 'e') {
                char path[160];
                int k = 0;

                while (q < inlen && (in[q] == ' ' || in[q] == '\t'))
                    q++;
                if (q < inlen && (in[q] == '"' || in[q] == '<')) {
                    char close = (in[q] == '"') ? '"' : '>';
                    unsigned char *tmp;
                    int fl, el, base;

                    q++;
                    strcpy(path, "/DOCS/");
                    base = 6;
                    while (q < inlen && in[q] != close && in[q] != '\n' && k < 150)
                        path[base + k++] = (char)in[q++];
                    path[base + k] = 0;
                    if (q < inlen && in[q] == close)
                        q++;
                    while (q < inlen && in[q] != '\n')
                        q++;
                    if (q < inlen)
                        q++;

                    tmp = (depth == 0) ? inc_buf0 : inc_buf1;
                    fl = load_file(path, tmp, SRC_MAX);
                    if (fl < 0) {
                        printf("tc: cannot open include %s\n", path);
                        return -1;
                    }
                    if (depth == 0) {
                        el = expand_includes(tmp, fl, out + o, cap - o, 1);
                        if (el < 0)
                            return -1;
                        o += el;
                    } else {
                        if (o + fl + 1 >= cap)
                            return -1;
                        memcpy(out + o, tmp, fl);
                        o += fl;
                        out[o++] = '\n';
                    }
                    i = q;
                    continue;
                }
            }
        }

        while (i < inlen && in[i] != '\n')
            out[o++] = in[i++];
        if (i < inlen)
            out[o++] = in[i++];
        if (o >= cap)
            return -1;
    }
    return o;
}

int main(void)
{
    char src_path[160];
    char out_path[160];
    int fd, n, r, total;

    src_path[0] = 0;
    out_path[0] = 0;
    sys_args(0, src_path, sizeof(src_path));
    if (!src_path[0])
        sys_args(1, src_path, sizeof(src_path));
    if (!src_path[0]) {
        printf("tc: usage: run /APPS/TC.APP <src.T> <out.APP>\n");
        return 1;
    }
    sys_args(1, out_path, sizeof(out_path));
    if (!out_path[0]) {
        printf("tc: no output path\n");
        return 1;
    }

    fd = sys_open(src_path, 0);
    if (fd < 0) {
        printf("tc: cannot open %s (%d)\n", src_path, fd);
        return 1;
    }
    n = 0;
    for (;;) {
        r = sys_read(fd, src_buf + n, SRC_MAX - n);
        if (r <= 0)
            break;
        n += r;
        if (n >= SRC_MAX)
            break;
    }
    sys_close(fd);
    printf("tc: source %s, %d bytes\n", src_path, n);

    /* 展开 #include */
    {
        int el = expand_includes(src_buf, n, exp_buf, (int)sizeof(exp_buf), 0);

        if (el < 0) {
            printf("tc: preprocess failed\n");
            return 1;
        }
        if (el != n)
            printf("tc: include expanded to %d bytes\n", el);
        fmac_collect(exp_buf, el);
        el = expand_func_macros(exp_buf, el, src_buf, (int)sizeof(src_buf));
        if (el < 0) {
            printf("tc: macro expansion failed\n");
            return 1;
        }
        if (nfmacs > 0)
            printf("tc: %d func-like macros expanded to %d bytes\n", nfmacs, el);
        n = el;
    }

    src = src_buf;
    src_len = n;
    src_pos = 0;
    cur_line = 1;
    code_len = 0;
    strs_len = 0;
    str_count = 0;
    str_patch_n = 0;
    call_patch_n = 0;
    nfuncs = 0;
    nvars = 0;
    ngvars = 0;
    nginit = 0;
    gdata_len = 0;

    /* 入口 stub：xor ebp,ebp; call __gin; call main; mov ebx,eax; xor eax,eax; int 0x80; hlt */
    e8(0x31); e8(0xED);
    e8(0xE8);
    call_patch[call_patch_n] = e32(0);
    strncpy(call_name[call_patch_n], "__gin", sizeof(call_name[0]) - 1);
    call_patch_n++;
    e8(0xE8);
    call_patch[call_patch_n] = e32(0);
    strncpy(call_name[call_patch_n], "main", sizeof(call_name[0]) - 1);
    call_patch_n++;
    e8(0x89); e8(0xC3);
    e8(0x31); e8(0xC0);
    e8(0xCD); e8(0x80);
    e8(0xF4);

    next();
    compile_unit();

    if (func_find("main") < 0)
        die("no main() function");
    if (funcs[func_find("main")].nparams != 0)
        die("main() must take no arguments");

    /* 全局初值：把所有常量初值在 main 之前写进全局数据区。
       做成一个独立函数 __gin，由入口 stub 先 call，避免和 ELF 段布局打交道。 */
    {
        int f = func_add("__gin", 0);
        int k;

        funcs[f].off = code_len;
        e8(0x55);                                    /* push ebp */
        e8(0x89); e8(0xE5);                          /* mov ebp,esp */
        for (k = 0; k < nginit; k++) {
            int a = CODE_BASE + GC_BASE + ginit[k].addr;

            if (ginit[k].size == 1) {
                e8(0xC6); e8(0x05); e32(a);
                e8(ginit[k].val & 0xFF);             /* mov byte [a],imm8 */
            } else if (ginit[k].size == 2) {
                e8(0x66); e8(0xC7); e8(0x05); e32(a);
                e8(ginit[k].val & 0xFF);
                e8((ginit[k].val >> 8) & 0xFF);      /* mov word [a],imm16 */
            } else {
                e8(0xC7); e8(0x05); e32(a);
                e32(ginit[k].val);                   /* mov dword [a],imm32 */
            }
        }
        e8(0x5D);                                    /* pop ebp */
        e8(0xC3);                                    /* ret */
        if (nginit > 0) {
            int q;

            printf("tc: %d global initializers written by __gin\n", nginit);
            for (q = 0; q < nginit; q++)
                printf("  gin[+%d] size %d val %d\n",
                       ginit[q].addr, ginit[q].size, ginit[q].val);
        }
    }

    for (int i = 0; i < call_patch_n; i++) {
        int f = func_find(call_name[i]);
        if (f < 0 || funcs[f].off < 0)
            die("undefined function");
        patch_rel(call_patch[i], funcs[f].off);
    }

    for (int i = 0; i < str_patch_n; i++) {
        int addr = CODE_BASE + HDR_SIZE + code_len + str_off[str_patch_id[i]];
        patch32(str_patch[i], addr);
    }

    total = build_elf(elf_buf, (int)sizeof(elf_buf));
    printf("tc: code %d bytes, strings %d bytes, globals %d bytes, elf %d bytes\n",
           code_len, strs_len, gdata_len, total);

#ifdef TC_DUMP
    for (int i = 0; i < nfuncs; i++)
        printf("tc: func %s @+%d\n", funcs[i].name, funcs[i].off);
    if (code_len <= 400) {
        printf("tc: code:");
        for (int i = 0; i < code_len; i++) {
            if (i % 16 == 0)
                printf("\n  %03x:", i);
            printf(" %02x", code[i]);
        }
        printf("\n");
    }
#endif

    fd = sys_open(out_path, 1);
    if (fd < 0) {
        printf("tc: cannot create %s (%d)\n", out_path, fd);
        return 1;
    }
    r = sys_filewrite(fd, elf_buf, total);
    if (r != total) {
        printf("tc: write failed (%d/%d)\n", r, total);
        sys_close(fd);
        return 1;
    }
    sys_close(fd);
    printf("tc: wrote %s\n", out_path);

    if (!sys_exists(out_path)) {
        printf("tc: output not visible to spawn\n");
        return 1;
    }

    r = sys_spawn(out_path, (const char *)0);
    if (r < 0) {
        printf("tc: spawn failed (%d)\n", r);
        return 1;
    }
    printf("tc: running %s as pid %d\n", out_path, r);
    {
        int st = sys_wait(r);
        printf("tc: program exited with %d\n", st);
        return st & 0xFF;
    }
}
