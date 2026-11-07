/* aptos 用户态最小 libm：Duktape 所需的 9 个数学函数 + 辅助。
 * 纯 C 实现（编译目标无硬件浮点指令时走软浮点），精度 ~1e-9，够 JS 用。 */
#include "std/math.h"

typedef union { double d; unsigned long long u; } dw_u;

double fabs(double x)
{
    dw_u u;
    u.d = x;
    u.u &= 0x7FFFFFFFFFFFFFFFull;
    return u.d;
}

double copysign(double x, double y)
{
    dw_u ux, uy;
    ux.d = x;
    uy.d = y;
    ux.u = (ux.u & 0x7FFFFFFFFFFFFFFFull) | (uy.u & 0x8000000000000000ull);
    return ux.d;
}

int isfinite(double x)
{
    dw_u u;
    u.d = x;
    return ((u.u >> 52) & 0x7FF) != 0x7FF;
}

int isnan(double x)
{
    dw_u u;
    u.d = x;
    return ((u.u >> 52) & 0x7FF) == 0x7FF && (u.u & 0xFFFFFFFFFFFFFull) != 0;
}

int isinf(double x)
{
    dw_u u;
    u.d = x;
    return ((u.u >> 52) & 0x7FF) == 0x7FF && (u.u & 0xFFFFFFFFFFFFFull) == 0;
}

double floor(double x)
{
    if (!isfinite(x))
        return x;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0)
        return x;  /* 整数域之外 */
    long long i = (long long)x;
    if ((double)i > x)
        i--;
    return (double)i;
}

double round(double x)
{
    if (!isfinite(x))
        return x;
    if (x >= 0) {
        double f = floor(x);
        return (x - f >= 0.5) ? f + 1.0 : f;
    }
    double f = floor(x);
    return (x - f <= -0.5) ? f : f + 1.0;
}

double ceil(double x)
{
    if (!isfinite(x))
        return x;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0)
        return x;
    long long i = (long long)x;
    if ((double)i < x)
        i++;
    return (double)i;
}

double trunc(double x)
{
    if (!isfinite(x))
        return x;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0)
        return x;
    long long i = (long long)x;
    return (double)i;
}

double fmod(double x, double y)
{
    if (y == 0.0 || !isfinite(x) || isnan(y))
        return 0.0 / 0.0;  /* NaN */
    if (fabs(y) > fabs(x))
        return x;
    if (!isfinite(y))
        return x;
    double q = floor(x / y);
    double r = x - q * y;
    if (r != 0.0 && (r < 0) != (x < 0))
        r += y;  /* 修正符号 */
    if (fabs(r) >= fabs(y)) {
        if ((r < 0) == (y < 0))
            r -= y;
        else
            r += y;
    }
    return r;
}

/* exp：x = n*ln2 + r，e^r 级数，再按 2^n 缩放 */
double exp(double x)
{
    if (x > 709.0)
        return 1e308;
    if (x < -745.0)
        return 0.0;
    int n = (int)(x / 0.6931471805599453 + (x >= 0 ? 0.5 : -0.5));
    double r = x - n * 0.6931471805599453;
    double sum = 1.0, term = 1.0;
    for (int k = 1; k <= 18; k++) {
        term *= r / k;
        sum += term;
    }
    dw_u u;
    u.d = sum;
    u.u += ((unsigned long long)n) << 52;
    return u.d;
}

/* log：x = m*2^e，m∈[1,2)，log(m) 用 atanh 级数 */
double log(double x)
{
    if (x <= 0.0)
        return 0.0 / 0.0;
    dw_u u;
    u.d = x;
    int e = (int)(((u.u >> 52) & 0x7FF) - 1023);
    u.u &= 0x800FFFFFFFFFFFFFull;
    u.u |= 0x3FF0000000000000ull;  /* m ∈ [1,2) */
    double m = u.d;
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double s = t, term = t;
    for (int k = 3; k <= 35; k += 2) {
        term *= t2;
        s += term / k;
    }
    return 2.0 * s + e * 0.6931471805599453;
}

/* sqrt：位技巧初值 + 牛顿迭代 */
double sqrt(double x)
{
    if (x < 0.0)
        return 0.0 / 0.0;
    if (x == 0.0)
        return 0.0;
    dw_u u;
    u.d = x;
    u.u = (u.u >> 1) + 0x1FF8000000000000ull;  /* 近似初值 */
    double g = u.d;
    for (int i = 0; i < 6; i++)
        g = 0.5 * (g + x / g);
    return g;
}

/* 多项式：sin/cos（泰勒，范围已归约到 [-pi,pi]） */
static double sin_poly(double x)
{
    double x2 = x * x;
    double sum = x, term = x;
    for (int n = 1; n <= 7; n++) {
        term *= -x2 / ((double)(2 * n) * (double)(2 * n + 1));
        sum += term;
    }
    return sum;
}

static double cos_poly(double x)
{
    double x2 = x * x;
    double sum = 1.0, term = 1.0;
    for (int n = 1; n <= 7; n++) {
        term *= -x2 / ((double)(2 * n - 1) * (double)(2 * n));
        sum += term;
    }
    return sum;
}

double sin(double x)
{
    x = fmod(x, 6.283185307179586);
    if (x > 3.141592653589793)
        x -= 6.283185307179586;
    else if (x < -3.141592653589793)
        x += 6.283185307179586;
    return sin_poly(x);
}

double cos(double x)
{
    x = fmod(x, 6.283185307179586);
    if (x > 3.141592653589793)
        x -= 6.283185307179586;
    else if (x < -3.141592653589793)
        x += 6.283185307179586;
    return cos_poly(x);
}

/* atan：|x|<=1 用级数，|x|>1 用 atan(x)=pi/2-atan(1/x) */
static double atan_impl(double x)
{
    int flip = 0;
    if (x > 1.0) {
        x = 1.0 / x;
        flip = 1;
    } else if (x < -1.0) {
        x = 1.0 / x;
        flip = -1;
    }
    double x2 = x * x;
    double sum = x, term = x;
    for (int k = 3; k <= 45; k += 2) {
        term *= -x2;
        sum += term / k;
    }
    if (flip == 1)
        return 1.5707963267948966 - sum;
    if (flip == -1)
        return -1.5707963267948966 - sum;
    return sum;
}

double atan(double x)
{
    return atan_impl(x);
}

double tan(double x)
{
    double c = cos(x);
    if (c == 0.0)
        return 0.0 / 0.0;  /* NaN */
    return sin(x) / c;
}

double asin(double x)
{
    if (x < -1.0 || x > 1.0)
        return 0.0 / 0.0;
    if (x == 1.0)
        return 1.5707963267948966;
    if (x == -1.0)
        return -1.5707963267948966;
    return atan_impl(x / sqrt(1.0 - x * x));
}

double acos(double x)
{
    if (x < -1.0 || x > 1.0)
        return 0.0 / 0.0;
    return 1.5707963267948966 - asin(x);
}

double log2(double x)
{
    return log(x) / 0.6931471805599453;
}

double log10(double x)
{
    return log(x) / 2.302585092994046;
}

double cbrt(double x)
{
    int neg = x < 0.0;
    double a = neg ? -x : x;
    if (a == 0.0)
        return x;
    /* 牛顿迭代 c = (2c + a/c^2)/3 */
    double c = exp(log(a) / 3.0);
    for (int i = 0; i < 4; i++)
        c = (2.0 * c + a / (c * c)) / 3.0;
    return neg ? -c : c;
}

double atan2(double y, double x)
{
    if (x == 0.0) {
        if (y > 0)
            return 1.5707963267948966;
        if (y < 0)
            return -1.5707963267948966;
        return 0.0 / 0.0;  /* NaN */
    }
    double a = atan_impl(y / x);
    if (x < 0.0)
        a = (y >= 0.0) ? a + 3.141592653589793 : a - 3.141592653589793;
    return a;
}

double pow(double x, double y)
{
    if (y == 0.0)
        return 1.0;
    if (x == 1.0)
        return 1.0;
    if (x < 0.0) {
        /* 仅整数指数有意义 */
        double ip = floor(y);
        if (ip != y)
            return 0.0 / 0.0;
        long long n = (long long)y;
        double r = 1.0, base = x;
        long long m = n < 0 ? -n : n;
        while (m) {
            if (m & 1)
                r *= base;
            base *= base;
            m >>= 1;
        }
        return n < 0 ? 1.0 / r : r;
    }
    return exp(y * log(x));
}
