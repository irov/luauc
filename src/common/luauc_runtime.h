// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#ifndef LUAUC_RUNTIME_H
#define LUAUC_RUNTIME_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define LUAU_NORETURN _Noreturn
#define LUAU_NOINLINE
#define LUAU_FORCEINLINE inline
#define LUAU_LIKELY(value) (value)
#define LUAU_UNLIKELY(value) (value)
#define LUAU_UNREACHABLE() abort()
#define LUAU_DEBUGBREAK() abort()
#define LUAU_FALLTHROUGH ((void)0)

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define LUAU_BIG_ENDIAN
#endif

typedef int (*luauc_assert_handler_t)(const char* expression, const char* file, int line, const char* function);

extern luauc_assert_handler_t luauc_assert_handler;
int luauc_assert_call_handler(const char* expression, const char* file, int line, const char* function);

#if !defined(NDEBUG) || defined(LUAU_ENABLE_ASSERT)
#define LUAU_ASSERT(expression) \
    ((void)(!!(expression) || (luauc_assert_call_handler(#expression, __FILE__, __LINE__, __func__) && (LUAU_DEBUGBREAK(), 0))))
#define LUAU_ASSERTENABLED
#else
#define LUAU_ASSERT(expression) ((void)sizeof(!!(expression)))
#endif

#define LUAU_FASTFLAG(flag) extern int FFlag_##flag;
#define LUAU_FASTFLAGVARIABLE(flag) int FFlag_##flag = 0;
#define LUAU_FASTINT(flag) extern int FInt_##flag;
#define LUAU_FASTINTVARIABLE(flag, default_value) int FInt_##flag = (default_value);

#define LUAU_DYNAMIC_FASTFLAG(flag) extern int DFFlag_##flag;
#define LUAU_DYNAMIC_FASTFLAGVARIABLE(flag, default_value) int DFFlag_##flag = (default_value);
#define LUAU_DYNAMIC_FASTINT(flag) extern int DFInt_##flag;
#define LUAU_DYNAMIC_FASTINTVARIABLE(flag, default_value) int DFInt_##flag = (default_value);

#define LUAU_FLAGVERSION(flag, version)

#endif
