/* gl3d v3: 实体着色 3D 演示 —— 深度缓冲 + Gouraud 插值
 * 双物体同屏旋转（实体立方体 + 实体金字塔，逐面颜色渐变）
 */
#include "aptos.h"
#include "gl.h"

/* 立方体 8 顶点 */
static const float cube_v[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};
/* 6 面（每面 2 三角形，逆时针向外） */
static const int cube_f[6][4] = {
    {0,1,2,3},  /* -Z 前 */
    {5,4,7,6},  /* +Z 后 */
    {4,0,3,7},  /* -X 左 */
    {1,5,6,2},  /* +X 右 */
    {3,2,6,7},  /* +Y 顶 */
    {4,5,1,0},  /* -Y 底 */
};
static const float cube_col[6][3] = {
    {1.0f, 0.4f, 0.4f}, {0.4f, 0.9f, 1.0f}, {0.5f, 1.0f, 0.5f},
    {1.0f, 0.9f, 0.4f}, {0.9f, 0.5f, 1.0f}, {0.7f, 0.7f, 0.9f},
};
/* 金字塔 4 顶点 + 4 三角面 */
static const float py_v[4][3] = {
    {0, 1.4f, 0}, { -1, -0.7f, 1}, {1, -0.7f, 1}, {0, -0.7f, -1.4f}
};
static const int py_f[4][3] = {
    {0,1,2}, {0,2,3}, {0,3,1}, {1,3,2}
};
static const float py_col[4][3] = {
    {1.0f, 0.55f, 0.35f}, {0.55f, 0.85f, 1.0f}, {0.9f, 0.8f, 0.3f}, {0.6f, 1.0f, 0.6f}
};

int main(void)
{
    int w = 460, h = 360;
    if (sys_win_open("GL3D - Solid", w, h) != 0)
        return 1;

    glViewport(0, 0, w, h - 24);
    glMatrixMode(0x1701 /*GL_PROJECTION*/);
    glLoadIdentity();
    glPerspective(60.0f, w / (float)(h - 24), 1.0f, 20.0f);
    glClearColor(0.09f, 0.12f, 0.16f);
    glClearDepth(1.0f);

    for (int frame = 0; frame < 90; frame++) {
        float ang = (float)frame * 4.0f;

        glClear(0x4000 /*GL_COLOR_BUFFER_BIT*/ | 0x0100 /*GL_DEPTH_BUFFER_BIT*/);
        sys_text(10, 6, "aptos GL3D v3 - solid + zbuffer + gouraud",
                 RGB(160, 200, 230), 1);

        /* 物体 1：实体立方体（左侧） */
        glMatrixMode(0x1700 /*GL_MODELVIEW*/);
        glLoadIdentity();
        glTranslatef(-1.6f, 0.0f, -6.0f);
        glRotatef(ang, 1.0f, 0.0f, 0.0f);
        glRotatef(ang * 0.7f, 0.0f, 1.0f, 0.0f);

        for (int f = 0; f < 6; f++) {
            glColor3f(cube_col[f][0], cube_col[f][1], cube_col[f][2]);
            glBegin(0x0004 /*GL_TRIANGLES*/);
            /* 四边形 -> 两个三角形（顶点色微调制造 Gouraud 渐变） */
            glVertex3f(cube_v[cube_f[f][0]][0], cube_v[cube_f[f][0]][1], cube_v[cube_f[f][0]][2]);
            glVertex3f(cube_v[cube_f[f][1]][0], cube_v[cube_f[f][1]][1], cube_v[cube_f[f][1]][2]);
            glVertex3f(cube_v[cube_f[f][2]][0], cube_v[cube_f[f][2]][1], cube_v[cube_f[f][2]][2]);
            glEnd();
            glBegin(0x0004);
            glVertex3f(cube_v[cube_f[f][0]][0], cube_v[cube_f[f][0]][1], cube_v[cube_f[f][0]][2]);
            glVertex3f(cube_v[cube_f[f][2]][0], cube_v[cube_f[f][2]][1], cube_v[cube_f[f][2]][2]);
            glVertex3f(cube_v[cube_f[f][3]][0], cube_v[cube_f[f][3]][1], cube_v[cube_f[f][3]][2]);
            glEnd();
        }

        /* 物体 2：实体金字塔（右侧） */
        glLoadIdentity();
        glTranslatef(1.7f, 0.0f, -6.0f);
        glRotatef(-ang * 0.6f, 0.0f, 1.0f, 0.0f);
        glRotatef(20.0f + ang * 0.3f, 1.0f, 0.0f, 0.0f);

        for (int f = 0; f < 4; f++) {
            glColor3f(py_col[f][0], py_col[f][1], py_col[f][2]);
            glBegin(0x0004);
            for (int k = 0; k < 3; k++) {
                const float *p = py_v[py_f[f][k]];
                glVertex3f(p[0], p[1], p[2]);
            }
            glEnd();
        }

        glFlush();
        sys_sleep(60);
    }
    return 0;
}
