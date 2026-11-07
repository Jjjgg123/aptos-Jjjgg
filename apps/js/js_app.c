/* aptos-node: Duktape JS 引擎运行时（uapp）
 * 本文件：最小可运行版 —— 建堆、console/print、跑 JS、输出结果 */
#include "duktape.h"
#include "aptos.h"

/* aptos.c malloc 日志引用 javm 的调试符号；js.app 无 javm，提供空定义 */
volatile const char *dbg_cls = 0;
volatile const char *dbg_mth = 0;

/* console.log / print：取栈顶转字符串打印 */
static duk_ret_t js_print(duk_context *ctx)
{
    int n = duk_get_top(ctx);
    int i;
    for (i = 0; i < n; i++) {
        const char *s = duk_safe_to_string(ctx, -n + i);
        if (i)
            printf(" ");
        printf("%s", s);
    }
    printf("\n");
    return 0;
}

/* console.error */
static duk_ret_t js_error(duk_context *ctx)
{
    int n = duk_get_top(ctx);
    int i;
    for (i = 0; i < n; i++) {
        const char *s = duk_safe_to_string(ctx, -n + i);
        if (i)
            printf(" ");
        printf("%s", s);
    }
    printf("\n");
    return 0;
}

/* ---- Node API: fs ---- */
static duk_ret_t js_fs_read(duk_context *ctx)
{
    const char *path = duk_require_string(ctx, 0);
    char buf[4096];
    int fd = sys_open(path, 0);
    if (fd < 0)
        return duk_error(ctx, DUK_ERR_ERROR, "fs: open failed: %s", path);
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    printf("[FS] read %d\n", n);
    if (n < 0)
        return duk_error(ctx, DUK_ERR_ERROR, "fs: read failed");
    buf[n] = 0;
    duk_push_string(ctx, buf);
    return 1;
}

static duk_ret_t js_fs_write(duk_context *ctx)
{
    const char *path = duk_require_string(ctx, 0);
    const char *data = duk_require_string(ctx, 1);
    int fd = sys_open(path, 1);   /* 写模式 */
    if (fd < 0) {
        printf("[FS] open fail %s\n", path);
        return duk_error(ctx, DUK_ERR_ERROR, "fs: create failed: %s", path);
    }
    int n = (int)strlen(data);
    int w = sys_filewrite(fd, data, n);
    sys_close(fd);
    printf("[FS] wrote %d/%d\n", w, n);
    if (w != n)
        return duk_error(ctx, DUK_ERR_ERROR, "fs: write failed");
    duk_push_int(ctx, n);
    return 1;
}

static duk_ret_t js_process_exit(duk_context *ctx)
{
    int code = (int)duk_require_int(ctx, 0);
    duk_destroy_heap(ctx);
    sys_exit(code);
    return 0;
}

static duk_ret_t js_process_cwd(duk_context *ctx)
{
    duk_push_string(ctx, "/");
    return 1;
}

static duk_ret_t js_process_version(duk_context *ctx)
{
    duk_push_string(ctx, "aptos-node 0.1.0 (duktape 2.7.0)");
    return 1;
}

/* 模块解析：原样 → +.js → +/index.js（最小 Node 语义） */
static int js_try_read(const char *path, char *buf, int bufsz)
{
    int fd = sys_open(path, 0);
    if (fd < 0)
        return -1;
    int n = sys_read(fd, buf, bufsz - 1);
    sys_close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    return n;
}

static int js_read_module(const char *path, char *buf, int bufsz)
{
    int n = js_try_read(path, buf, bufsz);
    if (n >= 0)
        return n;
    char cand[520];
    unsigned L = (unsigned)strlen(path);
    if (L < 480 && L + 4 < sizeof(cand)) {
        memcpy(cand, path, L);
        memcpy(cand + L, ".js", 4);
        n = js_try_read(cand, buf, bufsz);
        if (n >= 0)
            return n;
        if (L + 10 < sizeof(cand)) {
            memcpy(cand, path, L);
            memcpy(cand + L, "/index.js", 10);
            n = js_try_read(cand, buf, bufsz);
            if (n >= 0)
                return n;
        }
    }
    return -1;
}

/* require：CommonJS 最小语义（module/exports 注入、相对路径解析、模块缓存、返回 module.exports）
 * 模块内 require 遮蔽为相对路径版（基于 module.filename 目录），绝对路径走 global_require。 */
static duk_ret_t js_require(duk_context *ctx)
{
    const char *path = duk_require_string(ctx, 0);
    char buf[65536];
    int n = js_read_module(path, buf, sizeof(buf));
    if (n < 0)
        return duk_error(ctx, DUK_ERR_ERROR, "require: cannot open %s", path);

    /* 模块缓存：__aptos_module_cache[path] 命中直接返回 */
    duk_push_global_object(ctx);                 /* [G] */
    duk_get_prop_string(ctx, -1, "__aptos_module_cache"); /* [G] [C] */
    if (!duk_is_object(ctx, -1)) {
        duk_pop(ctx);                            /* [G] */
        duk_push_object(ctx);                    /* [G] [C] */
        duk_put_prop_string(ctx, -2, "__aptos_module_cache"); /* [G] */
        duk_get_prop_string(ctx, -1, "__aptos_module_cache"); /* [G] [C] */
    }
    duk_get_prop_string(ctx, -1, path);          /* [G] [C] [v] */
    if (!duk_is_undefined(ctx, -1)) {            /* 命中 */
        duk_remove(ctx, -2);                     /* [G] [v] */
        duk_remove(ctx, -2);                     /* [v] */
        return 1;
    }
    duk_pop(ctx);                                /* [G] [C] */
    duk_pop(ctx);                                /* [G] */
    duk_pop(ctx);                                /* [] */

    /* 预置缓存（循环依赖保护）：加载中命中返回空导出，加载完成后覆盖为真实导出 */
    duk_push_global_object(ctx);                 /* [G] */
    duk_get_prop_string(ctx, -1, "__aptos_module_cache"); /* [G] [C] */
    duk_push_object(ctx);                        /* [G] [C] [{}] */
    duk_push_string(ctx, path);                  /* [G] [C] [{}] [path] */
    duk_put_prop(ctx, -3);                       /* [G] [C]：C[path]={} */
    duk_pop(ctx);                                /* [G] */
    duk_pop(ctx);                                /* [] */

    /* module 对象 */
    duk_push_object(ctx);                           /* [module] */
    duk_push_object(ctx);                           /* [module] [{}] */
    duk_put_prop_string(ctx, -2, "exports");        /* module.exports = {} */
    duk_push_string(ctx, path);
    duk_put_prop_string(ctx, -2, "filename");
    duk_push_string(ctx, path);
    duk_put_prop_string(ctx, -2, "id");

    /* 包装函数：相对 require 解析 + 模块体 */
    duk_push_string(ctx,
        "(function(module, exports, require) {\n"
        "  var __dir = (module.filename || '/').split('/').slice(0, -1).join('/');\n"
        "  require = function(p) {\n"
        "    if (p.charAt(0) == '.') p = (__dir + '/' + p).replace(/\\/+/g, '/');\n"
        "    return global_require(p);\n"
        "  };\n");
    duk_push_string(ctx, buf);
    duk_push_string(ctx, "\n})");
    duk_concat(ctx, 3);
    duk_compile(ctx, DUK_COMPILE_EVAL);             /* [module] [fn] */

    /* 调用 fn(module, module.exports, global require) */
    duk_dup(ctx, -2);                               /* [module] [fn] [module] */
    duk_get_prop_string(ctx, -2, "exports");        /* [..] [fn] [module] [exports] */
    duk_push_global_object(ctx);                    /* [..] [fn] [module] [exports] [global] */
    duk_get_prop_string(ctx, -1, "require");        /* [..] [fn] [module] [exports] [global] [req] */
    duk_remove(ctx, -2);                            /* [..] [fn] [module] [exports] [req] */
    duk_call(ctx, 3);                               /* [module] [ret] */
    duk_pop(ctx);                                   /* [module] */

    duk_get_prop_string(ctx, -1, "exports");        /* [module] [ex] */

    /* 缓存写入：__aptos_module_cache[path] = ex */
    duk_push_global_object(ctx);                    /* [module] [ex] [G] */
    duk_get_prop_string(ctx, -1, "__aptos_module_cache"); /* [module] [ex] [G] [C] */
    if (!duk_is_object(ctx, -1)) {
        duk_pop(ctx);                               /* [module] [ex] [G] */
        duk_push_object(ctx);                       /* [module] [ex] [G] [C] */
        duk_put_prop_string(ctx, -2, "__aptos_module_cache"); /* [module] [ex] [G] */
        duk_get_prop_string(ctx, -1, "__aptos_module_cache"); /* [module] [ex] [G] [C] */
    }
    duk_dup(ctx, -3);                               /* [module] [ex] [G] [C] [ex] */
    duk_push_string(ctx, path);                     /* [..] [ex] [G] [C] [ex] [path] */
    duk_put_prop(ctx, -3);                          /* [..] [ex] [G] [C]：C[path]=ex */
    duk_pop(ctx);                                   /* [module] [ex] [G] */
    duk_pop(ctx);                                   /* [module] [ex] */
    duk_remove(ctx, -2);                            /* [ex] */
    return 1;
}

int main(int argc, char **argv)
{
    printf("[JS-RT v0.3] argc=%d\n", argc);
    printf("[js] creating heap...\n");
    duk_context *ctx = duk_create_heap_default();
    if (!ctx) {
        printf("js: heap create failed\n");
        return 1;
    }
    printf("[js] heap ok\n");
    printf("[js] malloc(64)=%p malloc(4096)=%p\n", malloc(64), malloc(4096));
    printf("[js] push_int test...\n");
    duk_push_int(ctx, 42);
    printf("[js] push_int ok, top=%d\n", duk_get_top(ctx));
    duk_pop(ctx);
    printf("[js] push_string test...\n");
    duk_push_string(ctx, "probe");
    printf("[js] push_string ok\n");
    duk_pop(ctx);
    printf("[js] push_object test...\n");
    duk_push_object(ctx);
    printf("[js] push_object ok\n");
    duk_pop(ctx);
    printf("[js] push_c_function...\n");
    printf("[js]  step1: push_c_function(print)...\n");
    duk_push_c_function(ctx, js_print, DUK_VARARGS);
    printf("[js]  step1 ok\n");
    duk_put_global_string(ctx, "print");
    printf("[js]  step2: put_global print ok\n");

    duk_push_object(ctx);                          /* console */
    duk_push_c_function(ctx, js_print, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "log");
    duk_push_c_function(ctx, js_error, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "error");
    duk_put_global_string(ctx, "console");

    /* fs 读/写：普通函数挂全局（lightfunc 指针编码在裸机不可用） */
    duk_push_c_function(ctx, js_fs_read, 1);
    duk_put_global_string(ctx, "__fs_read");
    duk_push_c_function(ctx, js_fs_write, 2);
    duk_put_global_string(ctx, "__fs_write");

    /* require：CommonJS 最小实现；global_require 供模块内相对解析回源 */
    duk_push_c_function(ctx, js_require, 1);
    duk_put_global_string(ctx, "require");
    duk_push_c_function(ctx, js_require, 1);
    duk_put_global_string(ctx, "global_require");

    /* 编译器可用性测试 + console/process/fs 全用 JS 定义（不经过 C put_prop） */
    printf("[js] peval test...\n");
    if (duk_peval_string(ctx, "1+2;") != 0) {
        printf("js: peval FAIL: %s\n", duk_safe_to_string(ctx, -1));
        return 1;
    }
    duk_pop(ctx);
    printf("[js] peval ok\n");

    if (duk_peval_string(ctx,
        "var console = { log: print, error: function(s){ print('ERR: '+s); } };\n"
        "var process = { major: 2, platform: 'aptos',\n"
        "  exit: function(c){ print('[exit] '+c); },\n"
        "  cwd: function(){ return '/'; },\n"
        "  version: function(){ return 'v2.7.0'; } };\n"
        "var fs = { readFileSync: __fs_read, writeFileSync: __fs_write };\n"
        "1;") != 0) {
        printf("js: builtin env FAIL: %s\n", duk_safe_to_string(ctx, -1));
        return 1;
    }
    duk_pop(ctx);
    printf("[js] builtin env ok\n");

    /* 内置模块 path/Buffer（最小语义：文本处理够用） */
    if (duk_peval_string(ctx,
        "(function(){\n"
        "  var path = {};\n"
        "  path.dirname = function(p){ p=String(p).replace(/\\/+$/,''); var i=p.lastIndexOf('/'); return i<=0?'/':p.slice(0,i); };\n"
        "  path.basename = function(p){ p=String(p).replace(/\\/+$/,''); var i=p.lastIndexOf('/'); return i<0?p:p.slice(i+1); };\n"
        "  path.join = function(){ var a=[]; for(var i=0;i<arguments.length;i++){ var s=String(arguments[i]); if(s) a.push(s); } return a.join('/').replace(/\\/+/g,'/'); };\n"
        "  path.resolve = path.join;\n"
        "  var Buffer = function(s){ this._s=String(s); this.length=this._s.length; };\n"
        "  Buffer.from = function(s){ return new Buffer(s); };\n"
        "  Buffer.prototype.toString = function(){ return this._s; };\n"
        "  globalThis.path = path; globalThis.Buffer = Buffer;\n"
        "})();") != 0) {
        printf("js: builtin module init error: %s\n", duk_safe_to_string(ctx, -1));
        duk_pop(ctx);
    }

    /* 执行脚本：argv[1] 为文件则读取，否则执行内嵌自测 */
    int rc = 0;
    if (argc > 1) {
        /* 简化：用 duk_push_string 读 argv[1] 指向的文件内容 */
        char buf[65536];
        int fd = sys_open(argv[1], 0);
        if (fd < 0) {
            printf("js: cannot open %s\n", argv[1]);
            return 1;
        }
        int n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n <= 0) {
            printf("js: empty file\n");
            return 1;
        }
        buf[n] = 0;
        if (duk_peval_string(ctx, buf) != 0) {
            printf("js error: %s\n", duk_safe_to_string(ctx, -1));
            rc = 1;
        }
        duk_pop(ctx);
    } else {
        /* 内嵌自测脚本（分段定位：seg1-4，每段独立 peval） */
        static const char *seg[4] = {
            /* seg1: 基础循环 + math + str */
            "var sum = 0;\n"
            "for (var i = 0; i < 6; i++) sum += i;\n"
            "print('js engine ok, 0..5 sum = ' + sum);\n"
            "print('math: floor(3.7)=' + Math.floor(3.7) + ' sqrt(2)=' + Math.sqrt(2).toFixed(6));\n"
            "print('str: ' + 'aptos'.toUpperCase() + ' len=' + 'aptos 2.0'.length);\n",
            /* seg2: 数组 sort 回调 + console */
            "var arr = [3,1,4,1,5]; arr.sort(function(a,b){return a-b;});\n"
            "print('sorted: ' + arr.join(','));\n"
            "console.log('console works');\n",
            /* seg3: Date + fs 读写 */
            "print('date: ' + new Date().toISOString().slice(0,10));\n"
            "fs.writeFileSync('/hello.txt', 'hello from node-style js\\n');\n"
            "print('fs write ok, read back: ' + fs.readFileSync('/hello.txt').trim());\n",
            /* seg4: process + path + buffer */
            "print('process: ' + process.version + ' cwd=' + process.cwd());\n"
            "print('path: ' + path.join('/a','b','c') + ' dir=' + path.dirname('/a/b/c') + ' base=' + path.basename('/a/b/c'));\n"
            "print('buffer: ' + Buffer.from('aptos-buf').toString() + ' len=' + Buffer.from('abc').length);\n"
        };
        for (int si = 0; si < 4; si++) {
            if (duk_peval_string(ctx, seg[si]) != 0) {
                printf("js error seg%d: %s\n", si + 1, duk_safe_to_string(ctx, -1));
                rc = 1;
            } else {
                printf("[js] seg%d ok\n", si + 1);
            }
            duk_pop(ctx);
        }
    }

    duk_destroy_heap(ctx);
    return rc;
}
