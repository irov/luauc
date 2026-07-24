// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LNUMUTILS_H
#define LUAUC_LNUMUTILS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define luai_numadd(a, b) ((a) + (b))
#define luai_numsub(a, b) ((a) - (b))
#define luai_nummul(a, b) ((a) * (b))
#define luai_numdiv(a, b) ((a) / (b))
#define luai_numpow(a, b) (pow(a, b))
#define luai_numunm(a) (-(a))
#define luai_numisnan(a) ((a) != (a))
#define luai_numeq(a, b) ((a) == (b))
#define luai_numlt(a, b) ((a) < (b))
#define luai_numle(a, b) ((a) <= (b))
#define luai_inteq(a, b) ((a) == (b))

static inline bool __luai_veceq(const float* a, const float* b)
{
#if LUA_VECTOR_SIZE == 4
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
#else
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
#endif
}

static inline bool __luai_vecisnan(const float* a)
{
#if LUA_VECTOR_SIZE == 4
    return a[0] != a[0] || a[1] != a[1] || a[2] != a[2] || a[3] != a[3];
#else
    return a[0] != a[0] || a[1] != a[1] || a[2] != a[2];
#endif
}

static inline float __luaui_signf(float v)
{
    return v > 0.0f ? 1.0f : v < 0.0f ? -1.0f : 0.0f;
}

static inline float __luaui_clampf(float v, float min, float max)
{
    float r = v < min ? min : v;
    return r > max ? max : r;
}

LUAU_FASTMATH_BEGIN
static inline double __luai_nummod(double a, double b)
{
    return a - floor(a / b) * b;
}
LUAU_FASTMATH_END

LUAU_FASTMATH_BEGIN
static inline double __luai_numidiv(double a, double b)
{
    return floor(a / b);
}
LUAU_FASTMATH_END

static inline float __luai_lerpf(float a, float b, float t)
{
    return (t == 1.0) ? b : a + (b - a) * t;
}

static inline int __luai_num2intvalue(double value)
{
    return value >= -2147483648.0 && value < 2147483648.0 ? (int)value : INT32_MIN;
}

static inline int64_t __luai_num2longvalue(double value)
{
    return value >= -9223372036854775808.0 && value < 9223372036854775808.0 ? (int64_t)value : INT64_MIN;
}

static inline unsigned __luai_num2unsignedvalue(double value)
{
    return value >= -9223372036854775808.0 && value < 9223372036854775808.0 ? (unsigned)(uint64_t)(int64_t)value : 0u;
}

#define luai_num2int(i, d) ((i) = __luai_num2intvalue(d))

#define luai_num2long(i, d) ((i) = luai_num2longvalue(d))

#define luai_num2unsigned(i, n) ((i) = __luai_num2unsignedvalue(n))

#define LUAI_MAXNUM2STR 48
#define LUAI_MAXINT2STR 30

LUAI_FUNC char* luai_num2str(char* buf, double n);
LUAI_FUNC char* luai_int2str(char* buf, int64_t n);

#define luai_str2num(s, p) strtod((s), (p))
#define luai_str2long(s, p, base) strtoll((s), (p), base)

#endif
