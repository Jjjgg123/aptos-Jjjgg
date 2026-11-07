#include "aptos.h"

/* fetch.app — 通过 HTTP 从宿主机拉文件进 aptos，避免为一个 .APP 重新烧盘。
 *
 * 用法：run /APPS/FETCH.APP <host> "<port> <urlpath> <localpath>"
 * 例：  run /APPS/FETCH.APP 192.168.10.104 "8000 /tc/tc.app /APPS/TC.APP"
 *
 * 注意：aptos 的 run 只把参数切成两段（arg0 / arg1），所以除了 host 之外
 * 的其余参数必须写在引号里当作一整串传进来，由 fetch 自己拆开。
 */

#define BUFSZ 8192
#define MAXBODY (16 * 1024 * 1024)

static char host[64];
static char rest[256];
static char upath[192];
static char local[192];
static char req[400];
static char buf[BUFSZ];

/* 取 s 里第 idx 个以空格分隔的 token；取到返回 1 */
static int tok_at(const char *s, int idx, char *out, int max)
{
    int i = 0;
    int n = 0;
    int k = 0;

    while (s[i]) {
        while (s[i] == ' ')
            i++;
        if (!s[i])
            break;
        if (n == idx) {
            while (s[i] && s[i] != ' ' && k < max - 1)
                out[k++] = s[i++];
            out[k] = 0;
            return 1;
        }
        while (s[i] && s[i] != ' ')
            i++;
        n++;
    }
    return 0;
}

static int find_hdr_end(const char *b, int len)
{
    int i;
    for (i = 0; i + 3 < len; i++)
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n')
            return i + 4;
    return -1;
}

static int parse_len(const char *b, int len)
{
    int i;
    for (i = 0; i + 16 < len; i++) {
        if (b[i] == 'C' && b[i + 1] == 'o' && b[i + 2] == 'n' &&
            b[i + 3] == 't' && b[i + 4] == 'e' && b[i + 5] == 'n' &&
            b[i + 6] == 't' && b[i + 7] == '-' && b[i + 8] == 'L' &&
            b[i + 9] == 'e' && b[i + 10] == 'n' && b[i + 11] == 'g' &&
            b[i + 12] == 't' && b[i + 13] == 'h') {
            int j = i + 13, v = 0;
            while (j < len && (b[j] < '0' || b[j] > '9'))
                j++;
            while (j < len && b[j] >= '0' && b[j] <= '9') {
                v = v * 10 + (b[j] - '0');
                j++;
            }
            return v;
        }
    }
    return -1;
}

static int is_ok(const char *b, int len)
{
    int i;
    for (i = 0; i + 3 < len; i++)
        if (b[i] == '2' && b[i + 1] == '0' && b[i + 2] == '0' && b[i + 3] == ' ')
            return 1;
    return 0;
}

int main(void)
{
    int i;
    int port = 8000;
    int conn;
    int fd = -1;
    int total = 0;
    int hdr;
    int expect = -1;
    int r;
    char tok[64];

    for (i = 0; i < 64; i++)
        host[i] = 0;
    for (i = 0; i < 256; i++)
        rest[i] = 0;
    for (i = 0; i < 192; i++)
        upath[i] = 0;
    for (i = 0; i < 192; i++)
        local[i] = 0;

    if (sys_args(0, host, 64) != 0) {
        printf("fetch: usage: fetch <host> \"<port> <url> <local>\"\n");
        return 1;
    }
    sys_args(1, rest, 256);

    if (tok_at(rest, 0, tok, 64)) {
        int v = 0;
        int k = 0;
        while (tok[k] >= '0' && tok[k] <= '9') {
            v = v * 10 + (tok[k] - '0');
            k++;
        }
        if (v > 0)
            port = v;
    }
    if (!tok_at(rest, 1, upath, 192)) {
        printf("fetch: missing url path\n");
        return 1;
    }
    if (!tok_at(rest, 2, local, 192)) {
        printf("fetch: missing local path\n");
        return 1;
    }

    printf("fetch: %s:%d%s -> %s\n", host, port, upath, local);
    conn = sys_tcp_connect(host, port);
    if (conn < 0) {
        printf("fetch: connect failed\n");
        return 2;
    }

    {
        int n = 0;
        char *p = req;
        const char *a = "GET ";
        const char *b2 = " HTTP/1.0\r\nHost: ";
        const char *c = "\r\nConnection: close\r\n\r\n";
        while (*a && n < 390) req[n++] = *a++;
        for (i = 0; upath[i] && n < 390; i++)
            req[n++] = upath[i];
        while (*b2 && n < 390) req[n++] = *b2++;
        for (i = 0; host[i] && n < 390; i++)
            req[n++] = host[i];
        while (*c && n < 390) req[n++] = *c++;
        req[n] = 0;
    }

    r = sys_tcp_send(req, strlen(req));
    if (r < 0) {
        printf("fetch: send failed\n");
        sys_tcp_close();
        return 3;
    }

    r = sys_tcp_recv(buf, BUFSZ, 3000);
    if (r <= 0) {
        printf("fetch: no response\n");
        sys_tcp_close();
        return 4;
    }
    if (!is_ok(buf, r)) {
        printf("fetch: server returned non-200\n");
        sys_tcp_close();
        return 5;
    }
    expect = parse_len(buf, r);
    hdr = find_hdr_end(buf, r);
    if (hdr < 0) {
        int r2 = sys_tcp_recv(buf + r, BUFSZ - r, 3000);
        if (r2 > 0) {
            r += r2;
            hdr = find_hdr_end(buf, r);
        }
    }
    if (hdr < 0) {
        printf("fetch: bad response header\n");
        sys_tcp_close();
        return 6;
    }

    fd = sys_open(local, 1);
    if (fd < 0) {
        printf("fetch: cannot create %s\n", local);
        sys_tcp_close();
        return 7;
    }

    if (r - hdr > 0) {
        int n = r - hdr;
        if (sys_filewrite(fd, buf + hdr, n) != n) {
            printf("fetch: write failed\n");
            sys_close(fd);
            sys_tcp_close();
            return 8;
        }
        total += n;
    }

    for (;;) {
        r = sys_tcp_recv(buf, BUFSZ, 3000);
        if (r <= 0)
            break;
        if (sys_filewrite(fd, buf, r) != r) {
            printf("fetch: write failed\n");
            sys_close(fd);
            sys_tcp_close();
            return 8;
        }
        total += r;
        if (total > MAXBODY)
            break;
    }

    sys_close(fd);
    sys_tcp_close();

    printf("fetch: got %d bytes\n", total);
    if (expect >= 0 && total != expect) {
        printf("fetch: size mismatch, expected %d\n", expect);
        return 9;
    }
    printf("fetch: ok\n");
    return 0;
}
