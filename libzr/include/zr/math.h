/* Math routines, implemented from first principles. */
#pragma once

#include "base.h"

#define PI 3.14159265358979323846
#define PI_F 3.14159265358979323846f
#define TAU_F 6.28318530717958647692f
#define INF (__builtin_inf())
#define NAN_F (__builtin_nanf(""))

f64 sqrt(f64 x);
f64 fabs(f64 x);
f64 floor(f64 x);
f64 ceil(f64 x);
f64 round(f64 x);
f64 trunc(f64 x);
f64 fmod(f64 x, f64 y);
f64 sin(f64 x);
f64 cos(f64 x);
f64 tan(f64 x);
f64 asin(f64 x);
f64 acos(f64 x);
f64 atan(f64 x);
f64 atan2(f64 y, f64 x);
f64 exp(f64 x);
f64 log(f64 x);
f64 log2(f64 x);
f64 log10(f64 x);
f64 pow(f64 x, f64 y);
f64 cbrt(f64 x);
f64 hypot(f64 x, f64 y);
f64 ldexp(f64 x, int e);
f64 frexp(f64 x, int *e);
bool is_nan(f64 x);
bool is_inf(f64 x);

/* Single-precision variants for graphics code. */
static inline f32 sqrtf(f32 x) { return __builtin_sqrtf(x); }
static inline f32 fabsf(f32 x) { return __builtin_fabsf(x); }
f32 floorf(f32 x);
f32 ceilf(f32 x);
f32 roundf(f32 x);
f32 fmodf(f32 x, f32 y);
f32 sinf(f32 x);
f32 cosf(f32 x);
f32 tanf(f32 x);
f32 atan2f(f32 y, f32 x);
f32 expf(f32 x);
f32 logf(f32 x);
f32 powf(f32 x, f32 y);

static inline f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
static inline f32 clampf(f32 x, f32 lo, f32 hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline f32 minf(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 maxf(f32 a, f32 b) { return a > b ? a : b; }

/* Animation easing curves, t in [0,1]. */
f32 ease_out_cubic(f32 t);
f32 ease_in_out_cubic(f32 t);
f32 ease_out_back(f32 t);
/* Critically damped spring step: moves *pos toward target, *vel carries momentum. */
void spring_step(f32 *pos, f32 *vel, f32 target, f32 stiffness, f32 dt);
