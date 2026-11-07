/* aptos 用户态 GL 库（软件渲染 v3）
 * 4x4 矩阵（列主序）、modelview+projection 两级、顶点流水线
 * 实体三角形光栅化：扫描线 + 深度缓冲（z-buffer）+ 顶点颜色插值（Gouraud）
 * 输出：用户态 backbuffer，glFlush 时整块 sys_blit 到窗口
 */
#include "gl.h"
#include "aptos.h"

extern void *malloc(unsigned size);

typedef float m4[16];

static m4  g_model, g_proj;      /* 两级矩阵 */
static m4 *g_cur = &g_model;     /* 当前矩阵指针 */

static int g_view_x, g_view_y, g_view_w, g_view_h;
static int g_begin_mode;
static int g_vcount;
static float g_vbuf[256][5];     /* 每顶点: clip x,y,z,w + packed color */
static unsigned g_cur_color;

/* 用户态帧缓冲 + 深度缓冲 */
static unsigned *g_fb;           /* view_w * view_h * 4 */
static float   *g_zb;            /* view_w * view_h * 4 */
static float    g_clear_r, g_clear_g, g_clear_b;
static float    g_clear_z = 1.0f;
static int      g_clear_color_dirty;

static void m4_id(m4 m)
{
    int i;
    for (i = 0; i < 16; i++)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(m4 out, const m4 a, const m4 b)
{
    m4 t;
    int r, c;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    }
    for (int i = 0; i < 16; i++)
        out[i] = t[i];
}

static void m4_apply(float *x, float *y, float *z, float *w, const m4 m)
{
    float vx = *x, vy = *y, vz = *z;
    *x = m[0] * vx + m[4] * vy + m[8] * vz + m[12];
    *y = m[1] * vx + m[5] * vy + m[9] * vz + m[13];
    *z = m[2] * vx + m[6] * vy + m[10] * vz + m[14];
    *w = m[3] * vx + m[7] * vy + m[11] * vz + m[15];
}

void glMatrixMode(int mode)
{
    g_cur = (mode == GL_PROJECTION) ? &g_proj : &g_model;
}

void glLoadIdentity(void)
{
    m4_id(*g_cur);
}

void glTranslatef(float x, float y, float z)
{
    m4 t;
    m4_id(t);
    t[12] = x; t[13] = y; t[14] = z;
    m4_mul(*g_cur, *g_cur, t);
}

static void m4_rot(m4 out, float ang, float x, float y, float z)
{
    float a = ang * 0.0174532925f;
    float a2 = a * a;
    float c = 1.0f - a2 / 2.0f + a2 * a2 / 24.0f;
    float s = a - a2 * a / 6.0f + a2 * a2 * a / 120.0f;
    m4_id(out);
    if (x != 0.0f) {
        out[5] = c; out[9] = s;
        out[6] = -s; out[10] = c;
    } else if (y != 0.0f) {
        out[0] = c; out[8] = -s;
        out[2] = s; out[10] = c;
    } else {
        out[0] = c; out[4] = -s;
        out[1] = s; out[5] = c;
    }
}

void glRotatef(float ang, float x, float y, float z)
{
    m4 r;
    m4_rot(r, ang, x, y, z);
    m4_mul(*g_cur, *g_cur, r);
}

void glPerspective(float fovy, float aspect, float znear, float zfar)
{
    float half = fovy * 0.5f * 0.0174532925f;
    float h2 = half * half;
    float h = half + h2 * half / 3.0f + 2.0f * h2 * h2 * half / 15.0f;
    float f = 1.0f / h;
    m4_id(*g_cur);
    (*g_cur)[0] = f / aspect;
    (*g_cur)[5] = f;
    (*g_cur)[10] = (zfar + znear) / (znear - zfar);
    (*g_cur)[11] = -1.0f;
    (*g_cur)[14] = (2.0f * zfar * znear) / (znear - zfar);
    (*g_cur)[15] = 0.0f;
}

void glOrtho(float l, float r, float b, float t, float n, float f)
{
    m4_id(*g_cur);
    (*g_cur)[0] = 2.0f / (r - l);
    (*g_cur)[5] = 2.0f / (t - b);
    (*g_cur)[10] = -2.0f / (f - n);
    (*g_cur)[12] = -(r + l) / (r - l);
    (*g_cur)[13] = -(t + b) / (t - b);
    (*g_cur)[14] = -(f + n) / (f - n);
}

void glViewport(int x, int y, int w, int h)
{
    g_view_x = x; g_view_y = y; g_view_w = w; g_view_h = h;
    if (!g_fb) {
        g_fb = (unsigned *)malloc((unsigned)(w * h * 4));
        g_zb = (float *)malloc((unsigned)(w * h * 4));
        g_clear_color_dirty = 1;
    }
}

void glClearColor(float r, float g, float b)
{
    g_clear_r = r; g_clear_g = g; g_clear_b = b;
    g_clear_color_dirty = 1;
}

void glClearDepth(float z)
{
    g_clear_z = z;
}

void glClear(unsigned bits)
{
    int n;
    if (!g_fb)
        return;
    if (bits & GL_COLOR_BUFFER_BIT) {
        unsigned col = RGB((int)(g_clear_r * 255.0f),
                           (int)(g_clear_g * 255.0f),
                           (int)(g_clear_b * 255.0f));
        n = g_view_w * g_view_h;
        for (int i = 0; i < n; i++)
            g_fb[i] = col;
        g_clear_color_dirty = 0;
    }
    if (bits & GL_DEPTH_BUFFER_BIT) {
        n = g_view_w * g_view_h;
        for (int i = 0; i < n; i++)
            g_zb[i] = g_clear_z;
    }
}

void glColor3f(float r, float g, float b)
{
    int ir = (int)(r * 255.0f), ig = (int)(g * 255.0f), ib = (int)(b * 255.0f);
    if (ir > 255) ir = 255;
    if (ig > 255) ig = 255;
    if (ib > 255) ib = 255;
    if (ir < 0) ir = 0;
    if (ig < 0) ig = 0;
    if (ib < 0) ib = 0;
    g_cur_color = ((unsigned)ir << 16) | ((unsigned)ig << 8) | (unsigned)ib;
}

void glBegin(int mode)
{
    g_begin_mode = mode;
    g_vcount = 0;
}

void glVertex3f(float x, float y, float z)
{
    if (g_vcount < 256) {
        float w = 1.0f;
        m4_apply(&x, &y, &z, &w, g_model);   /* 模型视图 */
        m4_apply(&x, &y, &z, &w, g_proj);    /* 投影 */
        g_vbuf[g_vcount][0] = x;
        g_vbuf[g_vcount][1] = y;
        g_vbuf[g_vcount][2] = z;
        g_vbuf[g_vcount][3] = w;
        g_vbuf[g_vcount][4] = (float)g_cur_color;
        g_vcount++;
    }
}

/* 深度缓冲 + Gouraud 三角形填充 */
static void tri_fill(const float *p0, const float *p1, const float *p2)
{
    int i0 = 0, i1 = 1, i2 = 2;   /* 按屏幕 y 排序的顶点索引 */
    int y0, y1, y2;
    float sxp[3], syp[3], zndc[3];

    /* 屏幕坐标 + NDC z */
    for (int k = 0; k < 3; k++) {
        const float *v = (k == 0) ? p0 : (k == 1) ? p1 : p2;
        float w = v[3];
        if (w == 0.0f)
            w = 1.0f;
        sxp[k] = g_view_x + (v[0] / w + 1.0f) * 0.5f * g_view_w;
        syp[k] = g_view_y + (1.0f - v[1] / w) * 0.5f * g_view_h;
        zndc[k] = v[2] / w;
    }
    if (syp[1] < syp[0]) { int t = i0; i0 = i1; i1 = t; }
    if (syp[2] < syp[i0]) { int t = i0; i0 = i2; i2 = t; }
    if (syp[2] < syp[i1]) { int t = i1; i1 = i2; i2 = t; }

    y0 = (int)syp[i0]; y1 = (int)syp[i1]; y2 = (int)syp[i2];
    if (y0 == y2)
        return; /* 退化 */
    if (y0 < 0) y0 = 0;
    if (y2 >= g_view_h) y2 = g_view_h - 1;

    {
        float sx[3], sy[3], zn[3];
        unsigned col[3];
        int idx[3] = { i0, i1, i2 };
        for (int k = 0; k < 3; k++) {
            int o = idx[k];
            sx[k] = sxp[o]; sy[k] = syp[o]; zn[k] = zndc[o];
            col[k] = (unsigned)(int)((k == 0) ? p0[4] : (k == 1) ? p1[4] : p2[4]);
        }
        /* 扫描线 */
        for (int yy = y0; yy <= y2; yy++) {
            float fy = (float)yy + 0.5f;
            float xa, xb, za, zb;
            int xl, xr;
            if (y1 > y0) {
                if (fy < (float)y1) {
                    float t = (fy - (float)y0) / (float)(y1 - y0);
                    xa = sx[0] + (sx[1] - sx[0]) * t;
                    za = zn[0] + (zn[1] - zn[0]) * t;
                } else {
                    float t = (fy - (float)y1) / (float)(y2 - y1);
                    xa = sx[1] + (sx[2] - sx[1]) * t;
                    za = zn[1] + (zn[2] - zn[1]) * t;
                }
            } else {
                float t = (fy - (float)y0) / (float)(y2 - y0);
                xa = sx[0] + (sx[2] - sx[0]) * t;
                za = zn[0] + (zn[2] - zn[0]) * t;
            }
            {
                float t = (fy - (float)y0) / (float)(y2 - y0);
                xb = sx[0] + (sx[2] - sx[0]) * t;
                zb = zn[0] + (zn[2] - zn[0]) * t;
            }
            if (xa > xb) { float tt = xa; xa = xb; xb = tt; tt = za; za = zb; zb = tt; }
            xl = (int)xa; xr = (int)xb;
            if (xl < 0) xl = 0;
            if (xr >= g_view_w) xr = g_view_w - 1;
            if (xl > xr)
                continue;
            for (int xx = xl; xx <= xr; xx++) {
                float fx = (float)xx + 0.5f;
                /* barycentric 权重 */
                float denom = (sy[1] - sy[2]) * (sx[0] - sx[2]) +
                              (sx[2] - sx[1]) * (sy[0] - sy[2]);
                float w0, w1, w2;
                float z;
                unsigned pixcol;
                int pix;
                if (denom == 0.0f)
                    continue;
                w0 = ((sy[1] - sy[2]) * (fx - sx[2]) +
                      (sx[2] - sx[1]) * (fy - sy[2])) / denom;
                w1 = ((sy[2] - sy[0]) * (fx - sx[2]) +
                      (sx[0] - sx[2]) * (fy - sy[2])) / denom;
                w2 = 1.0f - w0 - w1;
                if (w0 < -0.01f || w1 < -0.01f || w2 < -0.01f)
                    continue;
                z = w0 * zn[0] + w1 * zn[1] + w2 * zn[2];
                pix = yy * g_view_w + xx;
                if (z >= g_zb[pix])
                    continue;
                g_zb[pix] = z;
                {
                    float cr = (w0 * ((col[0] >> 16) & 0xFF) +
                                w1 * ((col[1] >> 16) & 0xFF) +
                                w2 * ((col[2] >> 16) & 0xFF));
                    float cg = (w0 * ((col[0] >> 8) & 0xFF) +
                                w1 * ((col[1] >> 8) & 0xFF) +
                                w2 * ((col[2] >> 8) & 0xFF));
                    float cb = (w0 * (col[0] & 0xFF) +
                                w1 * (col[1] & 0xFF) +
                                w2 * (col[2] & 0xFF));
                    int ir = (int)cr, ig = (int)cg, ib = (int)cb;
                    if (ir > 255) ir = 255; if (ig > 255) ig = 255; if (ib > 255) ib = 255;
                    pixcol = ((unsigned)ir << 16) | ((unsigned)ig << 8) | (unsigned)ib;
                }
                g_fb[pix] = pixcol;
            }
        }
    }
}

void glEnd(void)
{
    if (g_begin_mode == GL_LINES) {
        for (int i = 0; i + 1 < g_vcount; i += 2) {
            float sx0, sy0, sx1, sy1;
            float w0 = g_vbuf[i][3], w1 = g_vbuf[i + 1][3];
            if (w0 == 0.0f) w0 = 1.0f;
            if (w1 == 0.0f) w1 = 1.0f;
            sx0 = g_view_x + (g_vbuf[i][0] / w0 + 1.0f) * 0.5f * g_view_w;
            sy0 = g_view_y + (1.0f - g_vbuf[i][1] / w0) * 0.5f * g_view_h;
            sx1 = g_view_x + (g_vbuf[i + 1][0] / w1 + 1.0f) * 0.5f * g_view_w;
            sy1 = g_view_y + (1.0f - g_vbuf[i + 1][1] / w1) * 0.5f * g_view_h;
            sys_line((int)sx0, (int)sy0, (int)sx1, (int)sy1, g_cur_color);
        }
    } else if (g_begin_mode == GL_TRIANGLES) {
        for (int i = 0; i + 2 < g_vcount; i += 3)
            tri_fill(g_vbuf[i], g_vbuf[i + 1], g_vbuf[i + 2]);
    }
    g_vcount = 0;
}

void glFlush(void)
{
    if (g_fb)
        sys_blit(g_view_x, g_view_y, g_view_w, g_view_h, g_fb);
}
