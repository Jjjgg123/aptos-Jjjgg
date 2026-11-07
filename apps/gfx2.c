/* gfx2: 2D 渲染演示 —— 直线/圆/填充三角形/渐变/旋转多边形
 * 验证：通用显卡驱动 2D 路径全图元可用（用户态软件渲染 + 内核图元）
 * 整数定点查表旋转（无 libm，32 位裸机安全） */
#include "aptos.h"

#define WW 460
#define WH 360

/* sin 查表：角度 0-255 = 0-360 度，值域 4096 定点 */
#include "sintab.inc"

static int isin(int a) { return sintab[(a & 255)]; }
static int icos(int a) { return sintab[(a + 64) & 255]; }

static void line_aa(int x0, int y0, int x1, int y1, u32 c)
{
    sys_line(x0, y0, x1, y1, c);
}

/* 填充三角形（扫描线） */
static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, u32 c)
{
    int ys = y0, ye = y0;
    if (y1 < ys) ys = y1; if (y1 > ye) ye = y1;
    if (y2 < ys) ys = y2; if (y2 > ye) ye = y2;
    for (int y = ys; y <= ye; y++) {
        int l = 99999, r = -99999;
        int e0 = 0, e1 = 0, e2 = 0;
        if (y1 != y0)
            e0 = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        if (y2 != y1)
            e1 = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        if (y0 != y2)
            e2 = x2 + (x0 - x2) * (y - y2) / (y0 - y2);
        if (y >= y0 && y <= y1 && y1 != y0) { if (e0 < l) l = e0; if (e0 > r) r = e0; }
        if (y >= y1 && y <= y2 && y2 != y1) { if (e1 < l) l = e1; if (e1 > r) r = e1; }
        if (y >= y2 && y <= y0 && y0 != y2) { if (e2 < l) l = e2; if (e2 > r) r = e2; }
        if (l > r) { int t = l; l = r; r = t; }
        if (l < 0) l = 0;
        if (r >= WW) r = WW - 1;
        if (r >= l)
            sys_rect(l, y, r - l + 1, 1, c);
    }
}

/* 中点圆 */
static void circle(int cx, int cy, int r, u32 c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        sys_rect(cx + x, cy + y, 1, 1, c);
        sys_rect(cx + y, cy + x, 1, 1, c);
        sys_rect(cx - y, cy + x, 1, 1, c);
        sys_rect(cx - x, cy + y, 1, 1, c);
        sys_rect(cx - x, cy - y, 1, 1, c);
        sys_rect(cx - y, cy - x, 1, 1, c);
        sys_rect(cx + y, cy - x, 1, 1, c);
        sys_rect(cx + x, cy - y, 1, 1, c);
        y++;
        if (err < 0)
            err += 2 * y + 1;
        else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

/* 旋转多边形顶点（角度单位 256=360 度） */
static void poly_point(int a, int rad, int *x, int *y)
{
    *x = WW / 2 + (rad * icos(a)) / 4096;
    *y = WH / 2 - 30 + (rad * isin(a)) / 4096;
}

int main(void)
{
    int frame;

    if (sys_win_open("GFX2 - 2D Render", WW, WH) != 0)
        return 1;

    for (frame = 0; frame < 120; frame++) {
        int a = (frame * 3) & 255;   /* 旋转角 */
        int i;

        sys_clear(RGB(16, 20, 28));

        /* 渐变背景带 */
        for (i = 0; i < 24; i++)
            sys_rect(0, WH - 24 + i, WW, 1, RGB(12 + i * 3, 20 + i * 2, 30 + i));

        /* 1. 星形（旋转） */
        {
            int k;
            for (k = 0; k < 5; k++) {
                int xa, ya, xb, yb, xc, yc, xd, yd;
                poly_point(a + k * 51, 70, &xa, &ya);
                poly_point(a + k * 51 + 26, 30, &xb, &yb);
                poly_point(a + (k + 1) * 51, 70, &xc, &yc);
                poly_point(a + (k + 1) * 51 + 26, 30, &xd, &yd);
                line_aa(xa, ya, xb, yb, RGB(230, 200, 90));
                line_aa(xb, yb, xc, yc, RGB(230, 200, 90));
                line_aa(xc, yc, xd, yd, RGB(230, 200, 90));
            }
        }

        /* 2. 彩虹圆（呼吸半径） */
        {
            int rr = 30 + (8 * isin((frame * 2) & 255)) / 4096;
            circle(WW / 2, WH / 2 - 30, rr,
                   RGB(120 + 100 * isin((frame * 4) & 255) / 4096,
                       180 + 60 * isin((frame * 5) & 255) / 4096,
                       220 + 30 * isin((frame * 6) & 255) / 4096));
        }

        /* 3. 填充三角形（呼吸色） */
        {
            int cy = WH / 2 + 70;
            fill_tri(WW / 2, cy - 42, WW / 2 - 48, cy + 30, WW / 2 + 48, cy + 30,
                     RGB(40 + 120 * isin((frame * 3) & 255) / 4096,
                         60 + 100 * isin((frame * 4) & 255) / 4096, 200));
            line_aa(WW / 2, cy - 42, WW / 2 - 48, cy + 30, RGB(255, 255, 255));
            line_aa(WW / 2 - 48, cy + 30, WW / 2 + 48, cy + 30, RGB(255, 255, 255));
            line_aa(WW / 2 + 48, cy + 30, WW / 2, cy - 42, RGB(255, 255, 255));
        }

        /* 4. 旋转六边形 */
        {
            int k;
            for (k = 0; k < 6; k++) {
                int xa, ya, xb, yb;
                poly_point(256 - a + k * 43, 90, &xa, &ya);
                poly_point(256 - a + (k + 1) * 43, 90, &xb, &yb);
                line_aa(xa, ya, xb, yb, RGB(120, 220, 160));
            }
        }

        /* 5. 十字线 + 标题 */
        line_aa(0, WH / 2 - 30, WW, WH / 2 - 30, RGB(40, 50, 60));
        line_aa(WW / 2, 0, WW / 2, WH - 24, RGB(40, 50, 60));
        sys_text(8, 4, "aptos GFX2 - 2D render: lines/circle/tri/fill/gradient",
                 RGB(150, 200, 230), 1);
        sys_text(8, WH - 18, "spinning star + rainbow circle + filled tri + hexagon",
                 RGB(140, 160, 170), 1);

        sys_flush();
        sys_sleep(30);
    }
    return 0;
}
