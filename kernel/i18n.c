#include "i18n.h"
#include "fs.h"
#include "klog.h"
#include "util.h"

/* ---- 外部语言包：/I18N/<LANG>.MSG（安装时写入目标盘，英文默认、中文可选）
 * 格式：每行 key=text；'#' 开头为注释。文件优先于内置表。
 * 查询优先级：外部覆盖表 > 内置 TR 表 > key 本身。 */
#define I18N_MSG_FILE_MAX 16384
static char msgfile[I18N_MSG_FILE_MAX];
static char *ov_keys[I18N_MAX_KEYS];
static char *ov_vals[I18N_MAX_KEYS];
static int  ov_n;
static int  file_ok;

static void i18n_unescape(char *v)
{
    char *r = v, *w = v;
    while (*r) {
        if (r[0] == '\\' && r[1] == 'n') {
            *w++ = '\n';
            r += 2;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void i18n_load_file(const char *lang)
{
    char path[40];
    char *p;
    int len, i;

    ov_n = 0;
    file_ok = 0;

    /* 拼 /I18N/<LANG>.MSG（fs 路径直读接口） */
    i = 0;
    path[i++] = '/';
    path[i++] = 'I';
    path[i++] = '1';
    path[i++] = '8';
    path[i++] = 'N';
    path[i++] = '/';
    for (int k = 0; lang[k] && i < 30; k++)
        path[i++] = (lang[k] >= 'a' && lang[k] <= 'z') ? (char)(lang[k] - 'a' + 'A') : lang[k];
    path[i++] = '.';
    path[i++] = 'M';
    path[i++] = 'S';
    path[i++] = 'G';
    path[i] = 0;

    len = fs_read(path, msgfile, sizeof(msgfile) - 1);
    if (len <= 0)
        return;
    msgfile[len] = 0;

    p = msgfile;
    while (*p && ov_n < I18N_MAX_KEYS) {
        char *eol = strchr(p, '\n');
        int  ln = eol ? (int)(eol - p) : (int)strlen(p);
        char *s = p;
        char *eq;
        char *v;
        int vl;

        if (eol)
            p = eol + 1;
        else
            break;
        if (ln > 159)
            ln = 159;
        s[ln] = 0;
        while (*s == ' ' || *s == '\t' || *s == '\r')
            s++;
        if (!*s || *s == '#')
            continue;
        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        v = eq + 1;
        while (*v == ' ' || *v == '\t' || *v == '\r')
            v++;
        vl = (int)strlen(v);
        while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r'))
            v[--vl] = 0;
        if (!*s || !*v)
            continue;
        i18n_unescape(v);
        ov_keys[ov_n] = s;
        ov_vals[ov_n] = v;
        ov_n++;
    }
    file_ok = 1;
}

/* ---- 语言包表：任意语言在此扩展（新增语言列并注册） ---- */
static const char *const TR[][I18N_MAX_LANGS] = {
    /* key,                          en,                                    zh */
    { "ui.ok",                        "OK",                                  "确定" },
    { "ui.cancel",                    "Cancel",                              "取消" },
    { "ui.next",                      "Next>",                               "下一步>" },
    { "ui.back",                      "<Back",                               "<上一步" },
    { "ui.install",                   "Install",                             "安装" },
    { "ui.close",                     "Close",                               "关闭" },
    { "ui.open",                      "Open",                                "打开" },
    { "ui.file",                      "File",                                "文件" },
    { "ui.edit",                      "Edit",                                "编辑" },
    { "ui.help",                      "Help",                                "帮助" },
    { "ui.about",                     "About",                               "关于" },
    { "ui.view",                      "View",                                "查看" },
    { "ui.tools",                     "Tools",                               "工具" },
    { "ui.settings",                  "Settings",                            "设置" },
    { "ui.search",                    "Search",                              "搜索" },
    { "ui.save",                      "Save",                                "保存" },
    { "ui.delete",                    "Delete",                              "删除" },
    { "ui.rename",                    "Rename",                              "重命名" },
    { "ui.new",                       "New",                                 "新建" },
    { "ui.copy",                      "Copy",                                "复制" },
    { "ui.paste",                     "Paste",                               "粘贴" },
    { "ui.cut",                       "Cut",                                 "剪切" },
    { "ui.yes",                       "Yes",                                 "是" },
    { "ui.no",                        "No",                                  "否" },
    { "ui.error",                     "Error",                               "错误" },
    { "ui.warning",                   "Warning",                             "警告" },
    { "ui.info",                      "Information",                         "信息" },
    { "ui.done",                      "Done",                                "完成" },
    { "ui.retry",                     "Retry",                               "重试" },
    { "ui.failed",                    "Failed",                              "失败" },

    { "term.prompt",                  "aptos>",                              "aptos>" },
    { "term.notfound",                "command not found",                   "未找到命令" },
    { "term.badargs",                 "bad arguments",                       "参数错误" },
    { "term.toomany",                 "too many arguments",                  "参数过多" },
    { "term.nofile",                  "no such file or directory",           "没有此文件或目录" },
    { "term.noperm",                  "permission denied",                   "权限不足" },
    { "term.diskfull",                "disk full",                           "磁盘已满" },
    { "term.nospace",                 "out of memory",                       "内存不足" },

    { "setup.welcome.title",          "Welcome to the aptos 2.0 Setup Wizard",
                                      "欢迎使用 aptos 2.0 安装向导" },
    { "setup.welcome.body",           "This wizard will install aptos 2.0 on your computer.",
                                      "此向导将在您的计算机上安装 aptos 2.0。" },
    { "setup.language",               "Language / 语言",                     "语言 / Language" },
    { "setup.choose_lang",            "Press 1 for English, 2 for Chinese",
                                      "按 1 选择英文，按 2 选择中文" },
    { "setup.choose_disk",            "Select the destination disk",         "选择目标磁盘" },
    { "setup.choose_fs",              "Select the file system",              "选择文件系统" },
    { "setup.minifs",                 "MINIFS02 (native)",                   "MINIFS02（原生）" },
    { "setup.fat12",                  "FAT12",                               "FAT12" },
    { "setup.fat16",                  "FAT16",                               "FAT16" },
    { "setup.fat32",                  "FAT32",                               "FAT32" },
    { "setup.will_install",           "Click Install to begin.",            "点击“安装”开始。" },
    { "setup.installing",             "Installing aptos 2.0",               "正在安装 aptos 2.0" },
    { "setup.moment",                 "This may take a moment",             "这可能需要一点时间" },
    { "setup.working",                "Working...",                          "正在处理…" },
    { "setup.complete",               "aptos 2.0 has been installed.",       "aptos 2.0 安装完成。" },
    { "setup.reboot",                 "Remove the disc and reboot.",         "取出光盘并重新启动。" },
    { "setup.prepare",                "Preparing to install...",             "正在准备安装…" },
    { "setup.snapshot_read",          "Snapshot read: %d bytes, %d entries", "已读取系统快照：%d 字节，%d 项" },
    { "setup.boot_unpack",            "Boot image unpacked (%d bytes)",      "引导映像已解包（%d 字节）" },
    { "setup.boot_write",             "Boot loader and kernel written to sector 0",
                                      "引导程序和内核已写入第 0 扇区" },
    { "setup.fs_created",             "%s filesystem created at sector %d",  "%s 文件系统已创建于第 %d 扇区" },
    { "setup.folder",                 "Creating folder %s",                  "正在创建文件夹 %s" },
    { "setup.file",                   "Copying %s",                          "正在复制 %s" },
    { "setup.disc_detected",          "aptos setup disc detected",           "检测到 aptos 安装光盘" },
    { "setup.cd_mount_fail",          "cannot mount the CD-ROM",             "无法挂载光盘" },
    { "setup.lst_missing",            "SETUP.LST not found on the disc",     "光盘上未找到 SETUP.LST" },
    { "setup.exit",                   "setup exited with status %d",         "安装程序已退出，状态码 %d" },

    { "app.hello.title",              "Hello, aptos!",                       "你好，aptos！" },
    { "app.hello.body",               "This is the built-in hello application.",
                                      "这是系统内置的 hello 示例程序。" },
    { "app.about.title",              "About aptos",                         "关于 aptos" },
    { "app.about.body",               "aptos 2.0 - a tiny 32-bit x86 teaching OS",
                                      "aptos 2.0 —— 一个微型 32 位 x86 教学操作系统" },
    { "app.files.title",              "Files",                               "文件" },
    { "app.editor.title",             "Editor",                              "编辑器" },
    { "app.paint.title",              "Paint",                               "画图" },
    { "app.term.title",               "Terminal",                            "终端" },
    { "app.snake.title",              "Snake",                               "贪吃蛇" },
    { "app.mines.title",              "Minesweeper",                         "扫雷" },
    { "app.game2048.title",           "2048",                                "2048" },
    { "app.notepad.title",            "Notepad",                             "记事本" },
    { "app.calc.title",               "Calculator",                          "计算器" },
    { "app.clock.title",              "Clock",                               "时钟" },

    /* ---- 终端高频消息（app_terminal 通过 L() 查询；语言包文件可覆盖） ---- */
    { "term.welcome",      "aptos terminal - type 'help' for the command list.\n",
                             "aptos 终端 - 输入 'help' 查看命令列表。\n" },
    { "term.mounted",      "MINIFS02 is mounted at /, try: ls, cd /DOCS, pwd\n\n",
                             "MINIFS02 已挂载于 /，试试：ls, cd /DOCS, pwd\n\n" },
    { "term.usecat",       "usage: cat <file>\n",                           "用法：cat <文件>\n" },
    { "term.usewrite",     "usage: write <file> <text>\n",                  "用法：write <文件> <内容>\n" },
    { "term.usemkdir",     "usage: mkdir <name>\n",                         "用法：mkdir <目录名>\n" },
    { "term.userm",        "usage: rm <name>\n",                            "用法：rm <名称>\n" },
    { "term.usecp",        "usage: cp <source> <dest>\n",                   "用法：cp <源> <目标>\n" },
    { "term.usemv",        "usage: mv <source> <dest>\n",                   "用法：mv <源> <目标>\n" },
    { "term.useinstall",   "usage: install <package.lst>\n",                "用法：install <软件包.lst>\n" },
    { "term.usepkginfo",   "usage: pkginfo <package.lst>\n",                "用法：pkginfo <软件包.lst>\n" },
    { "term.useping",      "usage: ping <ip> [count]\n",                   "用法：ping <IP> [次数]\n" },
    { "term.usedns",       "usage: dns <hostname>\n",                      "用法：dns <主机名>\n" },
    { "term.userun",       "usage: run <executable> [args]\n",             "用法：run <程序> [参数]\n" },
    { "term.usemount",     "usage: mount <device> <mountpoint>\n",         "用法：mount <设备> <挂载点>\n" },
    { "term.useumount",    "usage: umount <mountpoint>\n",                 "用法：umount <挂载点>\n" },
    { "term.usemkfs",      "usage: mkfs <device> [minifs|fat12|fat16|fat32]\n",
                             "用法：mkfs <设备> [minifs|fat12|fat16|fat32]\n" },
    { "term.running",      "running %s in ring 3 (%d bytes)\n",            "正在运行 %s（ring 3，%d 字节）\n" },
    { "term.exited",       "process exited with status %d\n",              "进程已退出，状态 %d\n" },
    { "term.installfin",   "installation finished.\n",                     "安装完成。\n" },
    { "term.installfail",  "installation failed (%d)\n",                   "安装失败（%d）\n" },
    { "term.opening",      "opening package %s\n",                         "正在打开软件包 %s\n" },
    { "term.opened",       "opened %s\n",                                  "已打开 %s\n" },
    { "term.creatdir",     "created %s\n",                                 "已创建 %s\n" },
    { "term.removed",      "removed %s\n",                                 "已删除 %s\n" },
    { "term.copied",       "copied %s -> %s (%d bytes)\n",                 "已复制 %s -> %s（%d 字节）\n" },
    { "term.moved",        "moved %s -> %s\n",                             "已移动 %s -> %s\n" },
    { "term.wrote",        "wrote %d bytes to %s\n",                       "已写入 %d 字节到 %s\n" },
    { "term.reboot",       "rebooting...\n",                               "正在重启...\n" },
    { "term.poweroff",     "powering off via ACPI S5...\n",                "正在通过 ACPI S5 关机...\n" },
    { "term.ver",          "aptos 2.0 - 32-bit x86, C kernel, VBE framebuffer\n",
                             "aptos 2.0 - 32 位 x86，C 内核，VBE 帧缓冲\n" },
    { "term.notfoundline", "%s: %s (try 'help')\n",                        "%s：%s（试试 'help'）\n" },
    { "term.apps",         "applications:",                                "应用程序：" },
};

/* ---- 语言注册表（可扩展） ---- */
static const i18n_lang_t LANGS[] = {
    { "en", "English" },
    { "zh", "简体中文" },
};
#define LANG_COUNT (sizeof(LANGS) / sizeof(LANGS[0]))

static int cur = 0;  /* 索引到 LANGS */

void i18n_init(void)
{
    cur = 0;
    i18n_load_file("en");
}

int i18n_set(const char *id)
{
    for (int i = 0; i < (int)LANG_COUNT; i++) {
        if (strcmp(id, LANGS[i].id) == 0) {
            cur = i;
            i18n_load_file(LANGS[i].id);
            return 0;
        }
    }
    return -1;
}

const char *i18n_id(void)
{
    return LANGS[cur].id;
}

const char *i18n_native(void)
{
    return LANGS[cur].native;
}

int i18n_count(void)
{
    return (int)LANG_COUNT;
}

const i18n_lang_t *i18n_lang_at(int i)
{
    if (i < 0 || i >= (int)LANG_COUNT)
        return NULL;
    return &LANGS[i];
}

const char *i18n_tr(const char *key)
{
    int nkeys = (int)(sizeof(TR) / sizeof(TR[0]));
    if (key) {
        /* 1. 外部语言包文件（安装时可选写入）优先 */
        if (file_ok) {
            for (int i = 0; i < ov_n; i++) {
                if (strcmp(key, ov_keys[i]) == 0)
                    return ov_vals[i];
            }
        }
        /* 2. 内置表兜底 */
        for (int i = 0; i < nkeys; i++) {
            if (strcmp(key, TR[i][0]) == 0) {
                const char *t = TR[i][cur + 1];
                return (t && t[0]) ? t : key;
            }
        }
    }
    return key ? key : "";
}
