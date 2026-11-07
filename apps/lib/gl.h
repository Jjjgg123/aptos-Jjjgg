/* aptos 用户态 GL 库：最小 OpenGL 风格线框渲染（软件）
 * 矩阵栈 + 顶点变换 + 正交/透视投影 + 线框绘制（基于 sys_line）
 * 目标：3D API 调用不报错、可画线框 3D 场景 */
#ifndef APTOS_GL_H
#define APTOS_GL_H

/* 常量（对齐 OpenGL 语义） */
#define GL_LINES        0x0001
#define GL_TRIANGLES    0x0004
#define GL_MODELVIEW     0x1700
#define GL_PROJECTION    0x1701
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT  0x0100
#define GL_TRUE  1
#define GL_FALSE 0

void glClear(unsigned bits);
void glClearColor(float r, float g, float b);
void glClearDepth(float z);
void glBegin(int mode);
void glEnd(void);
void glVertex3f(float x, float y, float z);
void glColor3f(float r, float g, float b);
void glMatrixMode(int mode);
void glLoadIdentity(void);
void glTranslatef(float x, float y, float z);
void glRotatef(float ang, float x, float y, float z);
void glPerspective(float fovy, float aspect, float znear, float zfar);
void glOrtho(float l, float r, float b, float t, float n, float f);
void glViewport(int x, int y, int w, int h);
void glFlush(void);

#endif
