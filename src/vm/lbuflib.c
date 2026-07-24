// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lualib.h"

#include "lcommon.h"
#include "lbuffer.h"

#if defined(LUAU_BIG_ENDIAN)
#include <endian.h>
#endif

LUAU_FASTFLAG(LuauIntegerLibrary)

#include <string.h>

// while C API returns 'size_t' for binary compatibility in case of future extensions,
// in the current implementation, length and offset are limited to 31 bits
// because offset is limited to an integer, a single 64bit comparison can be used and will not overflow
#define isoutofbounds(offset, len, accessize) ((uint64_t)((unsigned)(offset)) + (accessize) > (uint64_t)(len))

_Static_assert(MAX_BUFFER_SIZE <= INT_MAX, "current implementation can't handle a larger limit");

#if defined(LUAU_BIG_ENDIAN)
static uint16_t __buffer_swap16(uint16_t value)
{
    return htole16(value);
}

static uint32_t __buffer_swap32(uint32_t value)
{
    return htole32(value);
}

static uint64_t __buffer_swap64(uint64_t value)
{
    return htole64(value);
}
#else
#define __buffer_swap16(value) (value)
#define __buffer_swap32(value) (value)
#define __buffer_swap64(value) (value)
#endif

static int __buffer_create(lua_State* L)
{
    int size = luaL_checkinteger(L, 1);

    luaL_argcheck(L, size >= 0, 1, "size");

    lua_newbuffer(L, size);
    return 1;
}

static int __buffer_fromstring(lua_State* L)
{
    size_t len = 0;
    const char* val = luaL_checklstring(L, 1, &len);

    void* data = lua_newbuffer(L, len);
    memcpy(data, val, len);
    return 1;
}

static int __buffer_tostring(lua_State* L)
{
    size_t len = 0;
    void* data = luaL_checkbuffer(L, 1, &len);

    lua_pushlstring(L, (char*)data, len);
    return 1;
}

#define DEFINE_BUFFER_INTEGER_FUNCTIONS(suffix, value_type, unsigned_type, swap_function) \
    static int __buffer_read_##suffix(lua_State* L) \
    { \
        size_t length = 0; \
        void* buffer = luaL_checkbuffer(L, 1, &length); \
        int offset = luaL_checkinteger(L, 2); \
        value_type value; \
        unsigned_type storage; \
        if (isoutofbounds(offset, length, sizeof(value))) \
            luaL_error(L, "buffer access out of bounds"); \
        memcpy(&storage, (char*)buffer + offset, sizeof(storage)); \
        storage = swap_function(storage); \
        memcpy(&value, &storage, sizeof(value)); \
        lua_pushnumber(L, (double)value); \
        return 1; \
    } \
    static int __buffer_write_##suffix(lua_State* L) \
    { \
        size_t length = 0; \
        void* buffer = luaL_checkbuffer(L, 1, &length); \
        int offset = luaL_checkinteger(L, 2); \
        value_type value = (value_type)luaL_checkunsigned(L, 3); \
        unsigned_type storage; \
        if (isoutofbounds(offset, length, sizeof(value))) \
            luaL_error(L, "buffer access out of bounds"); \
        memcpy(&storage, &value, sizeof(storage)); \
        storage = swap_function(storage); \
        memcpy((char*)buffer + offset, &storage, sizeof(storage)); \
        return 0; \
    }

DEFINE_BUFFER_INTEGER_FUNCTIONS(i8, int8_t, uint8_t, )
DEFINE_BUFFER_INTEGER_FUNCTIONS(u8, uint8_t, uint8_t, )
DEFINE_BUFFER_INTEGER_FUNCTIONS(i16, int16_t, uint16_t, __buffer_swap16)
DEFINE_BUFFER_INTEGER_FUNCTIONS(u16, uint16_t, uint16_t, __buffer_swap16)
DEFINE_BUFFER_INTEGER_FUNCTIONS(i32, int32_t, uint32_t, __buffer_swap32)
DEFINE_BUFFER_INTEGER_FUNCTIONS(u32, uint32_t, uint32_t, __buffer_swap32)

#undef DEFINE_BUFFER_INTEGER_FUNCTIONS

static int __buffer_readlong(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);

    if (isoutofbounds(offset, len, sizeof(uint64_t)))
        luaL_error(L, "buffer access out of bounds");

    int64_t val;
    memcpy(&val, (char*)buf + offset, sizeof(int64_t));

#if defined(LUAU_BIG_ENDIAN)
    {
        uint64_t storage;
        memcpy(&storage, &val, sizeof(storage));
        storage = __buffer_swap64(storage);
        memcpy(&val, &storage, sizeof(val));
    }
#endif

    lua_pushinteger64(L, val);
    return 1;
}

static int __buffer_writelong(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);
    int64_t value = luaL_checkinteger64(L, 3);

    if (isoutofbounds(offset, len, sizeof(int64_t)))
        luaL_error(L, "buffer access out of bounds");

#if defined(LUAU_BIG_ENDIAN)
    {
        uint64_t storage;
        memcpy(&storage, &value, sizeof(storage));
        storage = __buffer_swap64(storage);
        memcpy(&value, &storage, sizeof(value));
    }
#endif

    memcpy((char*)buf + offset, &value, sizeof(int64_t));
    return 0;
}

static int __buffer_read_f32(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);
    uint32_t storage;
    float value;

    if (isoutofbounds(offset, len, sizeof(value)))
        luaL_error(L, "buffer access out of bounds");

    memcpy(&storage, (char*)buf + offset, sizeof(storage));
    storage = __buffer_swap32(storage);
    memcpy(&value, &storage, sizeof(value));
    lua_pushnumber(L, (double)value);
    return 1;
}

static int __buffer_read_f64(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);
    uint64_t storage;
    double value;

    if (isoutofbounds(offset, len, sizeof(value)))
        luaL_error(L, "buffer access out of bounds");

    memcpy(&storage, (char*)buf + offset, sizeof(storage));
    storage = __buffer_swap64(storage);
    memcpy(&value, &storage, sizeof(value));
    lua_pushnumber(L, value);
    return 1;
}

static int __buffer_write_f32(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);
    float value = (float)luaL_checknumber(L, 3);
    uint32_t storage;

    if (isoutofbounds(offset, len, sizeof(value)))
        luaL_error(L, "buffer access out of bounds");

    memcpy(&storage, &value, sizeof(storage));
    storage = __buffer_swap32(storage);
    memcpy((char*)buf + offset, &storage, sizeof(storage));
    return 0;
}

static int __buffer_write_f64(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);
    double value = luaL_checknumber(L, 3);
    uint64_t storage;

    if (isoutofbounds(offset, len, sizeof(value)))
        luaL_error(L, "buffer access out of bounds");

    memcpy(&storage, &value, sizeof(storage));
    storage = __buffer_swap64(storage);
    memcpy((char*)buf + offset, &storage, sizeof(storage));
    return 0;
}

static int __buffer_readstring(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);
    int size = luaL_checkinteger(L, 3);

    luaL_argcheck(L, size >= 0, 3, "size");

    if (isoutofbounds(offset, len, ((unsigned)(size))))
        luaL_error(L, "buffer access out of bounds");

    lua_pushlstring(L, (char*)buf + offset, size);
    return 1;
}

static int __buffer_writestring(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);
    size_t size = 0;
    const char* val = luaL_checklstring(L, 3, &size);
    int count = luaL_optinteger(L, 4, ((int)(size)));

    luaL_argcheck(L, count >= 0, 4, "count");

    if (((size_t)(count)) > size)
        luaL_error(L, "string length overflow");

    // string size can't exceed INT_MAX at this point
    if (isoutofbounds(offset, len, ((unsigned)(count))))
        luaL_error(L, "buffer access out of bounds");

    memcpy((char*)buf + offset, val, count);
    return 0;
}

static int __buffer_len(lua_State* L)
{
    size_t len = 0;
    luaL_checkbuffer(L, 1, &len);

    lua_pushnumber(L, (double)(((unsigned)(len))));
    return 1;
}

static int __buffer_copy(lua_State* L)
{
    size_t tlen = 0;
    void* tbuf = luaL_checkbuffer(L, 1, &tlen);
    int toffset = luaL_checkinteger(L, 2);

    size_t slen = 0;
    void* sbuf = luaL_checkbuffer(L, 3, &slen);
    int soffset = luaL_optinteger(L, 4, 0);

    int size = luaL_optinteger(L, 5, ((int)(slen)) - soffset);

    if (size < 0)
        luaL_error(L, "buffer access out of bounds");

    if (isoutofbounds(soffset, slen, ((unsigned)(size))))
        luaL_error(L, "buffer access out of bounds");

    if (isoutofbounds(toffset, tlen, ((unsigned)(size))))
        luaL_error(L, "buffer access out of bounds");

    memmove((char*)tbuf + toffset, (char*)sbuf + soffset, size);
    return 0;
}

static int __buffer_fill(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int offset = luaL_checkinteger(L, 2);
    unsigned value = luaL_checkunsigned(L, 3);
    int size = luaL_optinteger(L, 4, ((int)(len)) - offset);

    if (size < 0)
        luaL_error(L, "buffer access out of bounds");

    if (isoutofbounds(offset, len, ((unsigned)(size))))
        luaL_error(L, "buffer access out of bounds");

    memset((char*)buf + offset, value & 0xff, size);
    return 0;
}

static int __buffer_readbits(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int64_t bitoffset = (int64_t)luaL_checknumber(L, 2);
    int bitcount = luaL_checkinteger(L, 3);

    if (bitoffset < 0)
        luaL_error(L, "buffer access out of bounds");

    if (((unsigned)(bitcount)) > 32)
        luaL_error(L, "bit count is out of range of [0; 32]");

    if ((uint64_t)(bitoffset + bitcount) > (uint64_t)(len) * 8)
        luaL_error(L, "buffer access out of bounds");

    unsigned startbyte = ((unsigned)(bitoffset / 8));
    unsigned endbyte = (unsigned)((bitoffset + bitcount + 7) / 8);

    uint64_t data = 0;

#if defined(LUAU_BIG_ENDIAN)
    for (int i = ((int)(endbyte)) - 1; i >= ((int)(startbyte)); i--)
        data = (data << 8) + (uint8_t)(((char*)buf)[i]);
#else
    memcpy(&data, (char*)buf + startbyte, endbyte - startbyte);
#endif

    uint64_t subbyteoffset = bitoffset & 0x7;
    uint64_t mask = (1ull << bitcount) - 1;

    lua_pushunsigned(L, (unsigned)((data >> subbyteoffset) & mask));
    return 1;
}

static int __buffer_writebits(lua_State* L)
{
    size_t len = 0;
    void* buf = luaL_checkbuffer(L, 1, &len);
    int64_t bitoffset = (int64_t)luaL_checknumber(L, 2);
    int bitcount = luaL_checkinteger(L, 3);
    unsigned value = luaL_checkunsigned(L, 4);

    if (bitoffset < 0)
        luaL_error(L, "buffer access out of bounds");

    if (((unsigned)(bitcount)) > 32)
        luaL_error(L, "bit count is out of range of [0; 32]");

    if ((uint64_t)(bitoffset + bitcount) > (uint64_t)(len) * 8)
        luaL_error(L, "buffer access out of bounds");

    unsigned startbyte = ((unsigned)(bitoffset / 8));
    unsigned endbyte = (unsigned)((bitoffset + bitcount + 7) / 8);

    uint64_t data = 0;

#if defined(LUAU_BIG_ENDIAN)
    for (int i = ((int)(endbyte)) - 1; i >= ((int)(startbyte)); i--)
        data = data * 256 + (uint8_t)(((char*)buf)[i]);
#else
    memcpy(&data, (char*)buf + startbyte, endbyte - startbyte);
#endif

    uint64_t subbyteoffset = bitoffset & 0x7;
    uint64_t mask = ((1ull << bitcount) - 1) << subbyteoffset;

    data = (data & ~mask) | (((uint64_t)(value) << subbyteoffset) & mask);

#if defined(LUAU_BIG_ENDIAN)
    for (int i = ((int)(startbyte)); i < ((int)(endbyte)); i++)
    {
        ((char*)buf)[i] = data & 0xff;
        data >>= 8;
    }
#else
    memcpy((char*)buf + startbyte, &data, endbyte - startbyte);
#endif
    return 0;
}

static const luaL_Reg __bufferlib[] = {
    {"create", __buffer_create},
    {"fromstring", __buffer_fromstring},
    {"tostring", __buffer_tostring},
    {"readi8", __buffer_read_i8},
    {"readu8", __buffer_read_u8},
    {"readi16", __buffer_read_i16},
    {"readu16", __buffer_read_u16},
    {"readi32", __buffer_read_i32},
    {"readu32", __buffer_read_u32},
    {"readf32", __buffer_read_f32},
    {"readf64", __buffer_read_f64},
    {"writei8", __buffer_write_i8},
    {"writeu8", __buffer_write_u8},
    {"writei16", __buffer_write_i16},
    {"writeu16", __buffer_write_u16},
    {"writei32", __buffer_write_i32},
    {"writeu32", __buffer_write_u32},
    {"writef32", __buffer_write_f32},
    {"writef64", __buffer_write_f64},
    {"readstring", __buffer_readstring},
    {"writestring", __buffer_writestring},
    {"len", __buffer_len},
    {"copy", __buffer_copy},
    {"fill", __buffer_fill},
    {"readbits", __buffer_readbits},
    {"writebits", __buffer_writebits},
    {"readinteger", __buffer_readlong},
    {"writeinteger", __buffer_writelong},
    {NULL, NULL},
};

static const luaL_Reg __bufferlib_NOINTEGER[] = {
    {"create", __buffer_create},
    {"fromstring", __buffer_fromstring},
    {"tostring", __buffer_tostring},
    {"readi8", __buffer_read_i8},
    {"readu8", __buffer_read_u8},
    {"readi16", __buffer_read_i16},
    {"readu16", __buffer_read_u16},
    {"readi32", __buffer_read_i32},
    {"readu32", __buffer_read_u32},
    {"readf32", __buffer_read_f32},
    {"readf64", __buffer_read_f64},
    {"writei8", __buffer_write_i8},
    {"writeu8", __buffer_write_u8},
    {"writei16", __buffer_write_i16},
    {"writeu16", __buffer_write_u16},
    {"writei32", __buffer_write_i32},
    {"writeu32", __buffer_write_u32},
    {"writef32", __buffer_write_f32},
    {"writef64", __buffer_write_f64},
    {"readstring", __buffer_readstring},
    {"writestring", __buffer_writestring},
    {"len", __buffer_len},
    {"copy", __buffer_copy},
    {"fill", __buffer_fill},
    {"readbits", __buffer_readbits},
    {"writebits", __buffer_writebits},
    {NULL, NULL},
};

int luaopen_buffer(lua_State* L)
{
    if (FFlag_LuauIntegerLibrary)
        luaL_register(L, LUA_BUFFERLIBNAME, __bufferlib);
    else
        luaL_register(L, LUA_BUFFERLIBNAME, __bufferlib_NOINTEGER);

    return 1;
}
