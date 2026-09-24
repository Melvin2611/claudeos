/* math library (double precision, SSE2 + x87 helpers) */
#include <math.h>
#include <stdint.h>

double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
float sqrtf(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
double fabs(double x) { union { double d; uint64_t u; } v = { x }; v.u &= ~(1ULL << 63); return v.d; }
float fabsf(float x) { return (float)fabs(x); }

double trunc(double x) {
    if (fabs(x) >= 4503599627370496.0 || x != x) return x;
    return (double)(int64_t)x;
}
double floor(double x) { double t = trunc(x); return (t > x) ? t - 1.0 : t; }
float floorf(float x) { return (float)floor(x); }
double ceil(double x) { double t = trunc(x); return (t < x) ? t + 1.0 : t; }
double round(double x) { return x >= 0 ? floor(x + 0.5) : -floor(-x + 0.5); }
double fmod(double x, double y) {
    if (y == 0) return NAN;
    double q = trunc(x / y);
    return x - q * y;
}
double modf(double x, double *ip) { double t = trunc(x); *ip = t; return x - t; }
double fmin(double a, double b) { return a < b ? a : b; }
double fmax(double a, double b) { return a > b ? a : b; }

/* x87 transcendental helpers */
double sin(double x) { double r; __asm__("fldl %1; fsin; fstpl %0" : "=m"(r) : "m"(x)); return r; }
double cos(double x) { double r; __asm__("fldl %1; fcos; fstpl %0" : "=m"(r) : "m"(x)); return r; }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
double tan(double x) { double r; __asm__("fldl %1; fptan; fstp %%st(0); fstpl %0" : "=m"(r) : "m"(x)); return r; }
double atan2(double y, double x) {
    double r;
    __asm__("fldl %1; fldl %2; fpatan; fstpl %0" : "=m"(r) : "m"(y), "m"(x));
    return r;
}
double atan(double x) { return atan2(x, 1.0); }
double asin(double x) { return atan2(x, sqrt(1.0 - x * x)); }
double acos(double x) { return atan2(sqrt(1.0 - x * x), x); }

double log2(double x) {
    double r;
    __asm__("fld1; fldl %1; fyl2x; fstpl %0" : "=m"(r) : "m"(x));
    return r;
}
double log(double x) { return log2(x) * 0.69314718055994530942; }
double log10(double x) { return log2(x) * 0.30102999566398119521; }

static double exp2_(double x) {
    /* 2^x = 2^int * 2^frac */
    double r;
    __asm__("fldl %1\n"
            "fld %%st(0)\n"
            "frndint\n"
            "fsub %%st, %%st(1)\n"
            "fxch\n"
            "f2xm1\n"
            "fld1\n"
            "faddp\n"
            "fscale\n"
            "fstp %%st(1)\n"
            "fstpl %0"
            : "=m"(r) : "m"(x));
    return r;
}
double exp(double x) { return exp2_(x * 1.44269504088896340736); }

double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return 0;
    if (y == trunc(y) && fabs(y) < 1e9) {
        long n = (long)y;
        int neg = n < 0;
        if (neg) n = -n;
        double r = 1, b = x;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1 / r : r;
    }
    if (x < 0) return NAN;
    return exp2_(y * log2(x));
}
double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x) { double e = exp(2 * x); return (e - 1) / (e + 1); }
double hypot(double x, double y) { return sqrt(x * x + y * y); }
double ldexp(double x, int e) { return x * pow(2.0, e); }
double frexp(double x, int *e) {
    if (x == 0) { *e = 0; return 0; }
    int ex = (int)floor(log2(fabs(x))) + 1;
    *e = ex;
    return x / pow(2.0, ex);
}
