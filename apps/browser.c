/*
 * aptos Browser 2.1 - firefox109-style system browser for aptos 2.0
 * - firefox-like chrome: toolbar with back/forward/reload/home + address bar
 * - about:home start page, about: pages (net, certs)
 * - HTML subset rendering with numbered link navigation
 * - HTTP/1.0 over the kernel TCP stack
 * - history stack (back/forward), Ctrl shortcuts
 */
#include "aptos.h"

static int strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return x - y;
        a++; b++;
    }
    return *a - *b;
}

static int strncasecmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n && a[i] && b[i]; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return x - y;
    }
    return 0;
}

static char *strstr(const char *h, const char *n)
{
    int i, j;
    if (!*n)
        return (char *)h;
    for (i = 0; h[i]; i++) {
        for (j = 0; n[j] && h[i + j]; j++)
            if (h[i + j] != n[j])
                break;
        if (!n[j])
            return (char *)&h[i];
    }
    return 0;
}

static char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == c)
            return (char *)s;
        s++;
    }
    return c == 0 ? (char *)s : 0;
}

#define WW 780
#define WH 540
#define BAR_H 30
#define TOP_Y  44
#define ROW_H  16
#define STAT_H 18
#define VIEW_ROWS ((WH - TOP_Y - STAT_H - 4) / ROW_H)

#define MAX_LINES 200
#define MAX_LINKS 40
#define MAX_HIST  32

typedef struct {
    char  text[80];
    int   link;      /* line carries a link */
    char  url[200];  /* link target */
} lline_t;

static lline_t lines[MAX_LINES];
static int nlines;
static int scroll;
static char addr[256];      /* address bar buffer */
static char cur_url[256];
static int net_ok;
static int mode;            /* 0=browse, 1=address */
static int link_rows[MAX_LINKS]; /* line index of n-th link */
static int nlinks;
static char hist_b[MAX_HIST][256];
static char hist_f[MAX_HIST][256];
static int hist_bn, hist_fn;

static void line_add(const char *t, const char *url)
{
    if (nlines >= MAX_LINES)
        return;
    strncpy(lines[nlines].text, t, 79);
    lines[nlines].text[79] = 0;
    lines[nlines].link = url ? 1 : 0;
    if (url) {
        strncpy(lines[nlines].url, url, 199);
        lines[nlines].url[199] = 0;
        if (nlinks < MAX_LINKS)
            link_rows[nlinks++] = nlines;
    }
    nlines++;
}

static void wrap_add(const char *t, const char *url)
{
    char tmp[120];
    int i = 0, n = 0;
    while (t[i]) {
        tmp[n++] = t[i];
        tmp[n] = 0;
        if (n >= 76) {
            line_add(tmp, url);
            n = 0;
        }
        i++;
    }
    if (n)
        line_add(tmp, url);
}

static char outbuf[8192];
static int outlen;

static void html_entity(char *o, int *oi, const char *e)
{
    if (!strncmp(e, "amp;", 4))      { o[(*oi)++] = '&'; }
    else if (!strncmp(e, "lt;", 3))  { o[(*oi)++] = '<'; }
    else if (!strncmp(e, "gt;", 3))  { o[(*oi)++] = '>'; }
    else if (!strncmp(e, "quot;", 5)){ o[(*oi)++] = '"'; }
    else if (!strncmp(e, "nbsp;", 5)){ o[(*oi)++] = ' '; }
}

/*
 * Render (possibly HTML) text into line list.
 * - strips tags, honours a subset: h1..h6, p, br, div, li, ul, ol,
 *   b/strong/i/em, a href, img alt, hr, center
 */
static void render_text(const char *src, int is_html)
{
    int i = 0;
    int in_a = 0;
    char atext[200], ahref[200];
    int an = 0, ah = 0;
    int indent = 0;
    int li_n = 0;
    int last_blank = 1;

    nlines = 0;
    nlinks = 0;
    outlen = 0;

    if (!is_html) {
        wrap_add(src, 0);
        return;
    }

    while (src[i]) {
        if (src[i] == '<') {
            char tag[64];
            int ti = 0;
            int j = i + 1;
            while (src[j] && src[j] != '>' && ti < 60) {
                tag[ti++] = src[j];
                j++;
            }
            tag[ti] = 0;
            if (src[j] == '>')
                i = j + 1;
            else
                i++;
            if (ti == 0)
                continue;

            if (!strncasecmp(tag, "a href", 6)) {
                char *h = strchr(tag, '"');
                if (h) {
                    char *e = strchr(h + 1, '"');
                    if (e) {
                        int n = (int)(e - h - 1);
                        if (n > 199) n = 199;
                        memcpy(ahref, h + 1, n);
                        ahref[n] = 0;
                        ah = n;
                    }
                }
                in_a = 1;
                atext[0] = 0;
                an = 0;
            } else if (!strncasecmp(tag, "/a", 2)) {
                if (in_a) {
                    char tmp[240];
                    snprintf(tmp, sizeof(tmp), "%s -> %s", atext, ahref);
                    line_add(tmp, ahref);
                    in_a = 0;
                }
            } else if (!strncasecmp(tag, "h1", 2) || !strncasecmp(tag, "h2", 2) ||
                       !strncasecmp(tag, "h3", 2) || !strncasecmp(tag, "h4", 2)) {
                line_add("=====", 0);
                indent = 0;
                last_blank = 0;
            } else if (!strncasecmp(tag, "br", 2) || !strncasecmp(tag, "p", 1) ||
                       !strncasecmp(tag, "/p", 2) || !strncasecmp(tag, "div", 3) ||
                       !strncasecmp(tag, "/div", 4)) {
                if (!last_blank) {
                    line_add("", 0);
                    last_blank = 1;
                }
            } else if (!strncasecmp(tag, "hr", 2)) {
                line_add("----------------------------------------", 0);
                last_blank = 0;
            } else if (!strncasecmp(tag, "center", 6)) {
                indent = 0;
            } else if (!strncasecmp(tag, "li", 2)) {
                li_n++;
                indent = 1;
            } else if (!strncasecmp(tag, "/li", 3)) {
                indent = 0;
            } else if (!strncasecmp(tag, "img", 3)) {
                char *al = strstr(tag, "alt");
                char tmp[80];
                if (al) {
                    char *h = strchr(al, '"');
                    if (h) {
                        char *e = strchr(h + 1, '"');
                        if (e) {
                            int n = (int)(e - h - 1);
                            if (n > 50) n = 50;
                            memcpy(tmp, h + 1, n);
                            tmp[n] = 0;
                            snprintf(tmp + n, 30, " [IMG]");
                        } else {
                            strcpy(tmp, "[IMG]");
                        }
                    } else {
                        strcpy(tmp, "[IMG]");
                    }
                } else {
                    strcpy(tmp, "[IMG]");
                }
                line_add(tmp, 0);
                last_blank = 0;
            }
            continue;
        }
        if (src[i] == '&') {
            html_entity(outbuf, &outlen, &src[i] + 1);
            if (outlen > 7000) {
                outbuf[outlen] = 0;
                wrap_add(outbuf, 0);
                outlen = 0;
            }
            i++;
            continue;
        }
        outbuf[outlen++] = src[i];
        if (src[i] == '\n') {
            outbuf[outlen - 1] = 0;
            if (outlen > 0) {
                if (indent) {
                    char tmp[100];
                    snprintf(tmp, sizeof(tmp), "  * %s", outbuf);
                    line_add(tmp, 0);
                } else {
                    line_add(outbuf, 0);
                }
                last_blank = 0;
            }
            outlen = 0;
        } else if (outlen >= 7000) {
            outbuf[outlen] = 0;
            wrap_add(outbuf, 0);
            outlen = 0;
        }
        if (in_a) {
            if (an < 190) {
                atext[an++] = src[i];
                atext[an] = 0;
            }
        }
        i++;
    }
    if (outlen) {
        outbuf[outlen] = 0;
        line_add(outbuf, 0);
    }
    if (in_a) {
        char tmp[240];
        snprintf(tmp, sizeof(tmp), "%s -> %s", atext, ahref);
        line_add(tmp, ahref);
    }
}

/* ---------------- HTTP ---------------- */
static u32 paton(const char *s)
{
    u32 r = 0;
    int i;
    for (i = 0; i < 4; i++) {
        u32 v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
        }
        r |= v << (8 * i);
        if (*s == '.')
            s++;
        else if (i < 3)
            return 0;
    }
    return r;
}

static void pntoa(u32 ip, char *out)
{
    snprintf(out, 32, "%d.%d.%d.%d", (u8)ip, (u8)(ip >> 8),
             (u8)(ip >> 16), (u8)(ip >> 24));
}

static int http_get(const char *host, int port, const char *path,
                    char *body, int bodycap, int *bodylen)
{
    char req[512];
    char ipstr[32];
    u32 ip;
    int total = 0, n, phase = 0;
    char buf[2048];
    int hdr_end = -1;

    if (net_ok && paton(host) != 0) {
        ip = paton(host);
    } else if (net_ok) {
        if (sys_dns_query(host, &ip) < 0)
            return -1;
    } else {
        return -1;
    }
    pntoa(ip, ipstr);

    if (sys_tcp_connect(ipstr, port) < 0)
        return -1;

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nUser-Agent: aptos-browser/2.1\r\n\r\n",
             path, host);
    if (sys_tcp_send(req, strlen(req)) < 0) {
        sys_tcp_close();
        return -1;
    }

    body[0] = 0;
    *bodylen = 0;
    while (1) {
        n = sys_tcp_recv(buf, sizeof(buf) - 1, 1200);
        if (n <= 0)
            break;
        buf[n] = 0;
        if (phase == 0) {
            char *he = strstr(buf, "\r\n\r\n");
            if (!he)
                he = strstr(buf, "\n\n");
            if (he) {
                hdr_end = (int)(he - buf);
                if (strncmp(buf, "HTTP/", 5) != 0) {
                    sys_tcp_close();
                    return -1;
                }
                phase = 1;
            }
        }
        if (phase == 1) {
            const char *p = buf;
            int skip = hdr_end >= 0 ? hdr_end + 4 : 0;
            if (skip < 0) skip = 0;
            if (skip > n)
                skip = n;
            p += skip;
            hdr_end = -1;
            while (*p && total < bodycap - 1) {
                body[total++] = *p++;
            }
        }
        if (total >= bodycap - 1)
            break;
    }
    body[total] = 0;
    *bodylen = total;
    sys_tcp_close();
    return 0;
}

/* ---------------- about pages ---------------- */
static void page_home(void)
{
    char b[1800];
    int n = 0;

    n += snprintf(b + n, sizeof(b) - n,
        "<center><h1>aptos 2.0</h1></center>"
        "<center><p>system browser - firefox109 edition</p></center>"
        "<br><center><p>Press Ctrl+L to type a URL, Ctrl+H home, Ctrl+B back, Ctrl+F forward, Ctrl+R reload</p></center>"
        "<br><p><a href=\"about:aptos\">about:aptos - system home</a></p>"
        "<p><a href=\"about:net\">about:net - network status</a></p>"
        "<p><a href=\"about:certs\">about:certs - Mozilla CA store</a></p>"
        "<p><a href=\"/DOCS/GUIDE/HOWTO.TXT\">/DOCS/GUIDE/HOWTO.TXT - user guide</a></p>"
        "<p><a href=\"/APPS/HELLO.CLASS\">/APPS/HELLO.CLASS - javm demo class</a></p>");
    render_text(b, 1);
}

static void page_aptos(void)
{
    char b[1600];
    int n = 0;

    n += snprintf(b + n, sizeof(b) - n,
        "<h1>aptos 2.0 Browser</h1>"
        "<p>Welcome to the aptos system browser (firefox109 edition).</p>"
        "<p>Shortcuts: Ctrl+L address, Ctrl+H home, Ctrl+B back, Ctrl+F forward, Ctrl+R reload, 0-9 open link.</p>"
        "<p>Examples:</p>"
        "<p><a href=\"about:home\">about:home - start page</a></p>"
        "<p><a href=\"about:net\">about:net - network status</a></p>"
        "<p><a href=\"about:certs\">about:certs - CA certificates</a></p>"
        "<p><a href=\"/DOCS/GUIDE/HOWTO.TXT\">/DOCS/GUIDE/HOWTO.TXT - local file</a></p>");
    render_text(b, 1);
}

static void page_net(void)
{
    char b[600];
    netinfo_t ni;
    int n = 0;

    if (sys_net_info(&ni) < 0) {
        render_text("network info unavailable", 0);
        return;
    }
    n += snprintf(b + n, sizeof(b) - n,
        "<h1>Network</h1>"
        "<p>MAC %02x:%02x:%02x:%02x:%02x:%02x</p>",
        ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    n += snprintf(b + n, sizeof(b) - n,
        "<p>IP %d.%d.%d.%d</p>", (u8)ni.ip, (u8)(ni.ip >> 8), (u8)(ni.ip >> 16), (u8)(ni.ip >> 24));
    n += snprintf(b + n, sizeof(b) - n,
        "<p>Gateway %d.%d.%d.%d</p>", (u8)ni.gw, (u8)(ni.gw >> 8), (u8)(ni.gw >> 16), (u8)(ni.gw >> 24));
    n += snprintf(b + n, sizeof(b) - n,
        "<p>DNS %d.%d.%d.%d</p>", (u8)ni.dns, (u8)(ni.dns >> 8), (u8)(ni.dns >> 16), (u8)(ni.dns >> 24));
    n += snprintf(b + n, sizeof(b) - n, "<p>Link %s</p>", ni.up ? "up" : "down");
    render_text(b, 1);
}

static void page_certs(void)
{
    char b[4096];
    int n = 0;
    int fd, len, i;
    static u8 certbuf[8192];

    fd = sys_open("/CERT.BIN", 0);
    if (fd < 0) {
        render_text("no CERT.BIN on system image", 0);
        return;
    }
    len = sys_read(fd, certbuf, sizeof(certbuf));
    sys_close(fd);
    if (len < 4) {
        render_text("CERT.BIN unreadable", 0);
        return;
    }
    n += snprintf(b + n, sizeof(b) - n,
        "<h1>Trusted Root CAs (Mozilla store)</h1>");
    {
        u32 count = (u32)certbuf[0] | ((u32)certbuf[1] << 8) |
                    ((u32)certbuf[2] << 16) | ((u32)certbuf[3] << 24);
        u32 off = 4;
        char tmp[300];
        for (i = 0; i < (int)count && i < 40 && off + 1 < (u32)len; i++) {
            int ll = certbuf[off];
            off += 1;
            if (off + ll + 20 > (u32)len)
                break;
            certbuf[off + ll] = 0;
            snprintf(tmp, sizeof(tmp), "<p>%s %02x%02x%02x%02x%02x%02x</p>",
                     (char *)&certbuf[off],
                     certbuf[off + ll], certbuf[off + ll + 1],
                     certbuf[off + ll + 2], certbuf[off + ll + 3],
                     certbuf[off + ll + 4], certbuf[off + ll + 5]);
            if (n + 200 < (int)sizeof(b))
                n += snprintf(b + n, sizeof(b) - n, "%s", tmp);
            off += ll + 20;
        }
    }
    render_text(b, 1);
}

/* ---------------- navigation ---------------- */
static void go_url(const char *url)
{
    if (hist_bn < MAX_HIST && cur_url[0] &&
        strcasecmp(hist_b[hist_bn > 0 ? hist_bn - 1 : 0], cur_url) != 0) {
        if (hist_bn < MAX_HIST)
            strncpy(hist_b[hist_bn++], cur_url, 255);
    } else if (hist_bn == 0) {
        strncpy(hist_b[hist_bn++], cur_url, 255);
    }
    hist_fn = 0;
    strncpy(cur_url, url, 255);
    cur_url[255] = 0;
}

static void navigate(const char *url)
{
    if (!url || !*url) {
        page_home();
        strcpy(cur_url, "about:home");
        return;
    }
    if (!strncasecmp(url, "about:home", 10)) {
        page_home();
        strcpy(cur_url, "about:home");
        return;
    }
    if (!strncasecmp(url, "about:aptos", 11)) {
        page_aptos();
        strcpy(cur_url, "about:aptos");
        return;
    }
    if (!strncasecmp(url, "about:net", 9)) {
        page_net();
        strcpy(cur_url, "about:net");
        return;
    }
    if (!strncasecmp(url, "about:certs", 11)) {
        page_certs();
        strcpy(cur_url, "about:certs");
        return;
    }
    if (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "HTTP://", 7)) {
        char host[128], path[200];
        int port = 80;
        const char *p = url + 7;
        int i = 0;
        while (*p && *p != '/' && *p != ':' && i < 120) {
            host[i++] = *p;
            p++;
        }
        host[i] = 0;
        if (*p == ':') {
            p++;
            port = 0;
            while (*p >= '0' && *p <= '9') {
                port = port * 10 + (*p - '0');
                p++;
            }
        }
        if (*p == '/') {
            int j = 0;
            while (*p && j < 190) {
                path[j++] = *p;
                p++;
            }
            path[j] = 0;
        } else {
            strcpy(path, "/");
        }
        char body[8192];
        int blen = 0;
        if (http_get(host, port, path, body, sizeof(body), &blen) == 0) {
            render_text(body, 1);
            go_url(url);
        } else {
            render_text("HTTP request failed (network down or host unreachable)", 0);
            go_url(url);
        }
        return;
    }
    /* local file */
    {
        int fd = sys_open((char *)url, 0);
        char b[8192];
        int len;
        if (fd < 0) {
            char msg[120];
            snprintf(msg, sizeof(msg), "cannot open %s", url);
            render_text(msg, 0);
            go_url(url);
            return;
        }
        len = sys_read(fd, b, sizeof(b) - 1);
        sys_close(fd);
        if (len < 0) {
            render_text("read error", 0);
        } else {
            b[len] = 0;
            render_text(b, 1);
        }
        go_url(url);
    }
}

static void go_back(void)
{
    if (hist_bn > 1) {
        if (hist_fn < MAX_HIST)
            strncpy(hist_f[hist_fn++], cur_url, 255);
        hist_bn--;
        navigate(hist_b[hist_bn - 1]);
        strcpy(addr, cur_url);
    }
}

static void go_forward(void)
{
    if (hist_fn > 0) {
        if (hist_bn < MAX_HIST)
            strncpy(hist_b[hist_bn++], cur_url, 255);
        hist_fn--;
        navigate(hist_f[hist_fn]);
        strcpy(addr, cur_url);
    }
}

/* ---------------- UI ---------------- */
static void draw(void)
{
    int i, y;
    char bar[320];
    char status[160];

    sys_clear(0x101418);
    /* toolbar */
    sys_rect(0, 0, WW, BAR_H, 0x1a3a5a);
    sys_rect(0, BAR_H - 1, WW, 2, 0x2a6a9a);
    /* nav buttons */
    sys_text(8, 6, "[<]B", 0xCFE6FF, 1);
    sys_text(44, 6, "[>]F", 0xCFE6FF, 1);
    sys_text(80, 6, "[R]d", 0xCFE6FF, 1);
    sys_text(116, 6, "[H]m", 0xCFE6FF, 1);
    /* address bar */
    sys_rect(150, 4, WW - 158, BAR_H - 8, 0x0E202C);
    sys_rect(150, 4, WW - 158, 1, 0x4a8ab0);
    snprintf(bar, sizeof(bar), "%s%s", mode == 1 ? "> " : "", addr);
    sys_text(154, 7, bar, 0xFFFFFF, 1);

    /* current url line */
    sys_text(8, BAR_H + 5, cur_url, 0x7FAFA0, 1);

    y = TOP_Y;
    for (i = scroll; i < nlines && y + ROW_H < WH - STAT_H; i++) {
        if (lines[i].link) {
            char lb[100];
            /* find link number */
            int n = 0, li;
            for (li = 0; li < nlinks; li++)
                if (link_rows[li] == i) { n = li; break; }
            snprintf(lb, sizeof(lb), "%d  %s", n, lines[i].text);
            sys_text(12, y, lb, 0x6FBBFF, 1);
        } else {
            sys_text(12, y, lines[i].text, 0xC8C8C8, 1);
        }
        y += ROW_H;
    }

    /* status bar */
    sys_rect(0, WH - STAT_H, WW, STAT_H, 0x0E1418);
    snprintf(status, sizeof(status), "%s | links %d | %s | 0-9 open  Ctrl+L addr  Ctrl+H home",
             net_ok ? "net up" : "net down", nlinks,
             mode == 1 ? "address" : "browse");
    sys_text(6, WH - STAT_H + 3, status, 0x8A9898, 1);

    sys_flush();
}

static void open_link(int n)
{
    if (n < 0 || n >= nlinks)
        return;
    navigate(lines[link_rows[n]].url);
    strcpy(addr, cur_url);
    scroll = 0;
    mode = 0;
    draw();
}

int main(void)
{
    char urlarg[256];
    int k;

    urlarg[0] = 0;
    sys_args(1, urlarg, sizeof(urlarg));

    sys_win_open("aptos Browser (firefox109)", WW, WH);

    net_ok = 0;
    {
        netinfo_t ni;
        if (sys_net_info(&ni) == (int)sizeof(netinfo_t) && ni.up)
            net_ok = 1;
    }

    mode = 0;
    addr[0] = 0;
    if (urlarg[0])
        navigate(urlarg);
    else
        navigate("about:home");
    strcpy(addr, cur_url);
    scroll = 0;
    draw();

    for (;;) {
        k = sys_getkey();
        if (k <= 0)
            continue;

        if (mode == 1) {
            /* address editing */
            if (k >= 0x20 && k < 0x7F) {
                int n = strlen(addr);
                if (n < 250) {
                    addr[n] = (char)k;
                    addr[n + 1] = 0;
                }
                draw();
            } else if (k == 8) {
                int n = strlen(addr);
                if (n > 0) {
                    addr[n - 1] = 0;
                    draw();
                }
            } else if (k == '\n' || k == 13) {
                navigate(addr);
                strcpy(addr, cur_url);
                scroll = 0;
                mode = 0;
                draw();
            } else if (k == 27) {
                mode = 0;
                strcpy(addr, cur_url);
                draw();
            }
            continue;
        }

        /* browse mode */
        if (k >= '0' && k <= '9') {
            open_link(k - '0');
        } else if (k == 12) { /* Ctrl+L */
            mode = 1;
            addr[0] = 0;
            draw();
        } else if (k == 8) { /* Ctrl+H (backspace == 8, also home) */
            if (addr[0]) {
                addr[0] = 0;
                draw();
            } else {
                navigate("about:home");
                strcpy(addr, cur_url);
                scroll = 0;
                draw();
            }
        } else if (k == 18) { /* Ctrl+R reload */
            navigate(cur_url);
            strcpy(addr, cur_url);
            scroll = 0;
            draw();
        } else if (k == 2) { /* Ctrl+B back */
            go_back();
            scroll = 0;
            draw();
        } else if (k == 6) { /* Ctrl+F forward */
            go_forward();
            scroll = 0;
            draw();
        } else if (k == 0x108) { /* PgUp */
            scroll -= 10;
            if (scroll < 0) scroll = 0;
            draw();
        } else if (k == 0x109) { /* PgDn */
            scroll += 10;
            if (scroll + VIEW_ROWS > nlines) scroll = nlines - VIEW_ROWS;
            if (scroll < 0) scroll = 0;
            draw();
        } else if (k == 0x101) { /* up */
            if (scroll > 0) {
                scroll--;
                draw();
            }
        } else if (k == 0x102) { /* down */
            if (scroll + VIEW_ROWS < nlines) {
                scroll++;
                draw();
            }
        } else if (k == 27) { /* esc */
            if (addr[0]) {
                addr[0] = 0;
                draw();
            } else {
                break;
            }
        } else if (k >= 0x20 && k < 0x7F) {
            /* any printable while browsing: start typing into address */
            mode = 1;
            addr[0] = 0;
            addr[1] = 0;
            if (k >= 0x20 && k < 0x7F) {
                addr[0] = (char)k;
                addr[1] = 0;
            }
            draw();
        }
    }
    return 0;
}
