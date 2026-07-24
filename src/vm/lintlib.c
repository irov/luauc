// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lualib.h"

#include "lcommon.h"
#include "lnumutils.h"
#include "lobject.h"

#include <limits.h>
#include <math.h>

LUAU_FASTFLAGVARIABLE(LuauIntegerLibrary)

#define mask64(w) (0xFFFFFFFFFFFFFFFFULL >> (64 - (w)))

static int __luauc_integer_count_trailing_zero(uint64_t value)
{
    int result = 0;
    if (value == 0)
        return 64;
    while ((value & UINT64_C(1)) == 0)
    {
        value >>= 1;
        result++;
    }
    return result;
}

static int __luauc_integer_count_leading_zero(uint64_t value)
{
    int result = 64;
    while (value != 0)
    {
        value >>= 1;
        result--;
    }
    return result;
}

static int __int64_create(lua_State* L)
{
    double x = luaL_checknumber(L, 1);
    if (x >= -9223372036854775808.0 && x < 9223372036854775808.0)
    {
        int64_t l = (int64_t)x;
        if (((double)l) == x)
        {
            lua_pushinteger64(L, l);
            return 1;
        }
    }

    lua_pushnil(L);

    return 1;
}

static int __int64_fromstring(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    int base = luaL_optinteger(L, 2, 10);
    luaL_argcheck(L, 2 <= base && base <= 36, 2, "base out of range");

    int64_t result;
    if (luaO_str2l(s, &result, base))
        lua_pushinteger64(L, result);
    else
        lua_pushnil(L);

    return 1;
}

static int __int64_tonumber(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);

    lua_pushnumber(L, (double)x);

    return 1;
}

static int __int64_neg(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);

    lua_pushinteger64(L, (int64_t)(~(uint64_t)x + 1));

    return 1;
}

static int __int64_add(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);
    int64_t y = luaL_checkinteger64(L, 2);

    lua_pushinteger64(L, (int64_t)((uint64_t)x + (uint64_t)y));

    return 1;
}

static int __int64_sub(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);
    int64_t y = luaL_checkinteger64(L, 2);

    lua_pushinteger64(L, (int64_t)((uint64_t)x - (uint64_t)y));

    return 1;
}

static int __int64_mul(lua_State* L)
{
    int64_t x = luaL_checkinteger64(L, 1);
    int64_t y = luaL_checkinteger64(L, 2);

    lua_pushinteger64(L, (int64_t)((uint64_t)x * (uint64_t)y));

    return 1;
}

static int __int64_div(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");
    if ((a == LLONG_MIN) && (b == -1))
        luaL_error(L, "integer overflow");

    lua_pushinteger64(L, a / b);

    return 1;
}

static int __int64_idiv(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");
    if ((a == LLONG_MIN) && (b == -1))
        luaL_error(L, "integer overflow");

    int64_t result = a / b;
    if ((result < 0) && (a % b))
        lua_pushinteger64(L, result - 1);
    else
        lua_pushinteger64(L, result);

    return 1;
}

static int __int64_rem(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");

    if ((a == LLONG_MIN) && (b == -1))
    {
        lua_pushinteger64(L, 0);
        return 1;
    }

    lua_pushinteger64(L, a % b);

    return 1;
}

static int __int64_mod(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");

    int64_t remainder = 0;
    if ((a != LLONG_MIN) || (b != -1))
    {
        remainder = a % b;
        if (remainder && ((a < 0) != (b < 0)))
            remainder += b;
    }

    lua_pushinteger64(L, remainder);

    return 1;
}

static int __int64_udiv(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");

    lua_pushinteger64(L, a / b);

    return 1;
}

static int __int64_urem(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    if (b == 0)
        luaL_error(L, "division by zero");

    lua_pushinteger64(L, a % b);

    return 1;
}

static int __int64_min(lua_State* L)
{
    int64_t tmin = luaL_checkinteger64(L, 1);
    int n = lua_gettop(L);
    for (int i = 2; i <= n; i++)
    {
        int64_t x = luaL_checkinteger64(L, i);
        if (x < tmin)
            tmin = x;
    }

    lua_pushinteger64(L, tmin);

    return 1;
}

static int __int64_max(lua_State* L)
{
    int64_t tmax = luaL_checkinteger64(L, 1);
    int n = lua_gettop(L);
    for (int i = 2; i <= n; i++)
    {
        int64_t x = luaL_checkinteger64(L, i);
        if (x > tmax)
            tmax = x;
    }

    lua_pushinteger64(L, tmax);

    return 1;
}

static int __int64_band(lua_State* L)
{
    uint64_t tres = ULLONG_MAX;
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++)
    {
        uint64_t x = (uint64_t)luaL_checkinteger64(L, i);
        tres &= x;
    }

    lua_pushinteger64(L, tres);

    return 1;
}

static int __int64_bor(lua_State* L)
{
    uint64_t tres = 0;
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++)
    {
        uint64_t x = (uint64_t)luaL_checkinteger64(L, i);
        tres |= x;
    }

    lua_pushinteger64(L, tres);

    return 1;
}

static int __int64_bnot(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);

    lua_pushinteger64(L, ~a);

    return 1;
}

static int __int64_bxor(lua_State* L)
{
    uint64_t tres = 0;
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++)
    {
        uint64_t x = (uint64_t)luaL_checkinteger64(L, i);
        tres ^= x;
    }

    lua_pushinteger64(L, tres);

    return 1;
}

static int __int64_lt(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a < b);

    return 1;
}

static int __int64_le(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a <= b);

    return 1;
}

static int __int64_ult(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a < b);

    return 1;
}

static int __int64_ule(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a <= b);

    return 1;
}

static int __int64_gt(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a > b);

    return 1;
}

static int __int64_ge(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a >= b);

    return 1;
}

static int __int64_ugt(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a > b);

    return 1;
}

static int __int64_uge(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);
    uint64_t b = luaL_checkinteger64(L, 2);

    lua_pushboolean(L, a >= b);

    return 1;
}

static int __int64_lshift(lua_State* L)
{
    uint64_t n = luaL_checkinteger64(L, 1);
    int64_t i = luaL_checkinteger64(L, 2);

    if ((i >= -63) && (i <= 63))
        lua_pushinteger64(L, (i < 0) ? (n >> (-i)) : (n << i));
    else
        lua_pushinteger64(L, 0);

    return 1;
}

static int __int64_rshift(lua_State* L)
{
    uint64_t n = luaL_checkinteger64(L, 1);
    int64_t i = luaL_checkinteger64(L, 2);

    if ((i >= -63) && (i <= 63))
        lua_pushinteger64(L, (i < 0) ? (n << (-i)) : (n >> i));
    else
        lua_pushinteger64(L, 0);

    return 1;
}

static int __int64_arshift(lua_State* L)
{
    int64_t n = luaL_checkinteger64(L, 1);
    int64_t i = luaL_checkinteger64(L, 2);

    if ((i >= -63) && (i <= 63))
        lua_pushinteger64(L, (i < 0) ? (int64_t)((uint64_t)n << (-i)) : (n >> i));
    else if (i < -63)
        lua_pushinteger64(L, 0);
    else
        lua_pushinteger64(L, (n < 0) ? -1 : 0);

    return 1;
}

static int __int64_lrotate(lua_State* L)
{
    uint64_t n = (uint64_t)luaL_checkinteger64(L, 1);
    unsigned s = (unsigned)((uint64_t)luaL_checkinteger64(L, 2) % 64);

    lua_pushinteger64(L, (int64_t)(s != 0 ? (n << s) | (n >> (64 - s)) : n));

    return 1;
}

static int __int64_rrotate(lua_State* L)
{
    uint64_t n = (uint64_t)luaL_checkinteger64(L, 1);
    unsigned s = (unsigned)((uint64_t)luaL_checkinteger64(L, 2) % 64);

    lua_pushinteger64(L, (int64_t)(s != 0 ? (n >> s) | (n << (64 - s)) : n));

    return 1;
}

static int __int64_extract(lua_State* L)
{
    int64_t n = luaL_checkinteger64(L, 1);
    int64_t f = luaL_checkinteger64(L, 2);
    int64_t w = luaL_optinteger64(L, 3, 1);

    luaL_argcheck(L, 0 <= f && f <= 63, 2, "field cannot be negative");
    luaL_argcheck(L, 0 < w, 3, "width must be positive");
    if (f + w > 64)
        luaL_error(L, "trying to access non-existent bits");

    lua_pushinteger64(L, ((uint64_t)n >> f) & mask64(w));

    return 1;
}

static int __int64_replace(lua_State* L)
{
    int64_t n = luaL_checkinteger64(L, 1);
    int64_t r = luaL_checkinteger64(L, 2);
    int64_t f = luaL_checkinteger64(L, 3);
    int64_t w = luaL_optinteger64(L, 4, 1);

    luaL_argcheck(L, 0 <= f && f <= 63, 3, "field cannot be negative");
    luaL_argcheck(L, 0 < w, 4, "width must be positive");
    if (f + w > 64)
        luaL_error(L, "trying to access non-existent bits");

    uint64_t baseMask = ((0xFFFFFFFFFFFFFFFFULL) >> (64 - w));
    uint64_t replacement = (((uint64_t)r) & baseMask) << f;
    uint64_t mask = 0xFFFFFFFFFFFFFFFFULL ^ (baseMask << f);
    lua_pushinteger64(L, (((uint64_t)n) & mask) | replacement);

    return 1;
}

static int __int64_clamp(lua_State* L)
{
    int64_t a = luaL_checkinteger64(L, 1);
    int64_t mi = luaL_checkinteger64(L, 2);
    int64_t mx = luaL_checkinteger64(L, 3);

    luaL_argcheck(L, mi <= mx, 3, "max must be greater than or equal to min");

    if (a < mi)
        lua_pushinteger64(L, mi);
    else if (a > mx)
        lua_pushinteger64(L, mx);
    else
        lua_pushinteger64(L, a);

    return 1;
}

static int __int64_btest(lua_State* L)
{
    uint64_t tres = ULLONG_MAX;
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++)
    {
        uint64_t x = (uint64_t)luaL_checkinteger64(L, i);
        tres &= x;
    }

    lua_pushboolean(L, (tres != 0));

    return 1;
}

static int __int64_countrz(lua_State* L)
{
    uint64_t n = luaL_checkinteger64(L, 1);
    int result;
    result = __luauc_integer_count_trailing_zero(n);

    lua_pushinteger64(L, result);

    return 1;
}

static int __int64_countlz(lua_State* L)
{
    uint64_t n = luaL_checkinteger64(L, 1);
    int result;
    result = __luauc_integer_count_leading_zero(n);
    lua_pushinteger64(L, result);

    return 1;
}

static int __int64_bswap(lua_State* L)
{
    uint64_t a = luaL_checkinteger64(L, 1);

    lua_pushinteger64(
        L,
        (a >> 56) | ((a & 0xFF000000000000) >> 40) | ((a & 0xFF0000000000) >> 24) | ((a & 0xFF00000000) >> 8) | ((a & 0xFF000000) << 8) |
            ((a & 0xFF0000) << 24) | ((a & 0xFF00) << 40) | ((a & 0xFF) << 56)
    );

    return 1;
}

static const luaL_Reg __int64lib[] = {
    {"create", __int64_create},
    {"tonumber", __int64_tonumber},
    {"neg", __int64_neg},
    {"add", __int64_add},
    {"sub", __int64_sub},
    {"mul", __int64_mul},
    {"div", __int64_div},
    {"min", __int64_min},
    {"max", __int64_max},
    {"rem", __int64_rem},
    {"idiv", __int64_idiv},
    {"udiv", __int64_udiv},
    {"urem", __int64_urem},
    {"mod", __int64_mod},
    {"clamp", __int64_clamp},
    {"band", __int64_band},
    {"bor", __int64_bor},
    {"bnot", __int64_bnot},
    {"bxor", __int64_bxor},
    {"lt", __int64_lt},
    {"le", __int64_le},
    {"ult", __int64_ult},
    {"ule", __int64_ule},
    {"gt", __int64_gt},
    {"ge", __int64_ge},
    {"ugt", __int64_ugt},
    {"uge", __int64_uge},
    {"lshift", __int64_lshift},
    {"rshift", __int64_rshift},
    {"arshift", __int64_arshift},
    {"lrotate", __int64_lrotate},
    {"rrotate", __int64_rrotate},
    {"extract", __int64_extract},
    {"replace", __int64_replace},
    {"btest", __int64_btest},
    {"countrz", __int64_countrz},
    {"countlz", __int64_countlz},
    {"bswap", __int64_bswap},
    {"fromstring", __int64_fromstring},
    {NULL, NULL},
};

int luaopen_integer(lua_State* L)
{
    luaL_register(L, LUA_INTLIBNAME, __int64lib);

    lua_pushinteger64(L, LLONG_MAX);
    lua_setfield(L, -2, "maxsigned");
    lua_pushinteger64(L, LLONG_MIN);
    lua_setfield(L, -2, "minsigned");

    return 1;
}
