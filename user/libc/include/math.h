#pragma once

#define M_PI 3.14159265358979323846
#define M_E 2.7182818284590452354
#define HUGE_VAL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)

double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log10(double x);
double log2(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double hypot(double x, double y);
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float sinf(float x);
float cosf(float x);
double fmin(double a, double b);
double fmax(double a, double b);
double modf(double x, double *ip);
double ldexp(double x, int e);
double frexp(double x, int *e);
