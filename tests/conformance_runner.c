// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include "luauc_bytecode_utils.h"
#include "lobject.h"
#include "lstate.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int FFlag_LuauYieldIter2;
extern int FFlag_LuauCustomYieldablePcalls;
extern int FFlag_LuauIntegerLibrary;
extern int FFlag_DebugLuauUserDefinedClassesRuntime;
extern int FFlag_LuauUdataDirectAccess6;
extern int DFFlag_LuauGcTableStepFix;

static int __conformance_allocations_allowed = 1;
static int __conformance_break_hits = 0;
static lua_State* __conformance_interrupted_thread = NULL;

static int __conformance_collectgarbage(lua_State* state)
{
    static const char* const __names[] = {
        "stop", "restart", "collect", "count", "isrunning", "step",
        "setgoal", "setstepmul", "setstepsize", NULL
    };
    static const int __operations[] = {
        LUA_GCSTOP, LUA_GCRESTART, LUA_GCCOLLECT, LUA_GCCOUNT, LUA_GCISRUNNING,
        LUA_GCSTEP, LUA_GCSETGOAL, LUA_GCSETSTEPMUL, LUA_GCSETSTEPSIZE
    };
    int option = luaL_checkoption(state, 1, "collect", __names);
    int argument = luaL_optinteger(state, 2, 0);
    int result = lua_gc(state, __operations[option], argument);
    if (__operations[option] == LUA_GCSTEP || __operations[option] == LUA_GCISRUNNING)
        lua_pushboolean(state, result);
    else
        lua_pushnumber(state, result);
    return 1;
}

static int __conformance_loadstring(lua_State* state)
{
    size_t source_size = 0;
    size_t bytecode_size = 0;
    const char* source = luaL_checklstring(state, 1, &source_size);
    const char* chunkname = luaL_optstring(state, 2, source);
    char* bytecode;
    int status;

    lua_setsafeenv(state, LUA_ENVIRONINDEX, 0);
    bytecode = luau_compile(source, source_size, NULL, &bytecode_size);
    if (bytecode == NULL)
    {
        luaL_error(state, "compiler allocation failure");
        return 0;
    }
    status = luau_load(state, chunkname, bytecode, bytecode_size, 0);
    free(bytecode);
    if (status == 0)
        return 1;
    lua_pushnil(state);
    lua_insert(state, -2);
    return 2;
}

static int __conformance_silent_print(lua_State* state)
{
    (void)state;
    return 0;
}

static int __conformance_makelud(lua_State* state)
{
    if (lua_type(state, 1) == LUA_TNUMBER)
    {
        unsigned int value = luaL_checkunsigned(state, 1);
        lua_pushlightuserdata(state, (void*)(uintptr_t)value);
    }
    else
    {
        const void* value = lua_topointer(state, 1);
        if (value == NULL)
        {
            luaL_error(state, "%s", "makelud expects a collectable value");
            return 0;
        }
        lua_pushlightuserdata(state, (void*)value);
    }
    return 1;
}

static int __conformance_c_yielding_iterator_continuation(lua_State* state, int status)
{
    int index = luaL_checkinteger(state, 2);
    (void)status;
    lua_pushinteger(state, index + 1);
    lua_pushinteger(state, index + 1);
    return 2;
}

static int __conformance_c_yielding_iterator(lua_State* state)
{
    int maximum = luaL_checkinteger(state, 1);
    int index = luaL_checkinteger(state, 2);

    if (index >= maximum)
        return 0;

    lua_pushinteger(state, index + 1);
    return lua_yield(state, 1);
}

static int __conformance_is_native(lua_State* state)
{
    lua_pushboolean(state, 0);
    return 1;
}

static int __conformance_is_native_if_supported(lua_State* state)
{
    lua_pushboolean(state, 1);
    return 1;
}

static void __conformance_coverage_callback(
    void* context,
    const char* function,
    int line_defined,
    int depth,
    const int* hits,
    size_t size
)
{
    lua_State* state = (lua_State*)context;
    size_t index;

    lua_newtable(state);
    lua_pushstring(state, function);
    lua_setfield(state, -2, "name");
    lua_pushinteger(state, line_defined);
    lua_setfield(state, -2, "linedefined");
    lua_pushinteger(state, depth);
    lua_setfield(state, -2, "depth");

    for (index = 0; index < size; ++index)
    {
        if (hits[index] != -1)
        {
            lua_pushinteger(state, hits[index]);
            lua_rawseti(state, -2, (int)index);
        }
    }

    lua_rawseti(state, -2, (int)lua_objlen(state, -2) + 1);
}

static int __conformance_getcoverage(lua_State* state)
{
    luaL_argexpected(state, lua_isLfunction(state, 1), 1, "function");
    lua_newtable(state);
    lua_getcoverage(state, 1, state, __conformance_coverage_callback);
    return 1;
}

static int __conformance_vector_dot(lua_State* state)
{
    const float* left = luaL_checkvector(state, 1);
    const float* right = luaL_checkvector(state, 2);
    lua_pushnumber(
        state,
        left[0] * right[0] + left[1] * right[1] + left[2] * right[2]
    );
    return 1;
}

static int __conformance_vector_cross(lua_State* state)
{
    const float* left = luaL_checkvector(state, 1);
    const float* right = luaL_checkvector(state, 2);
#if LUA_VECTOR_SIZE == 4
    lua_pushvector(
        state,
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
        0.0f
    );
#else
    lua_pushvector(
        state,
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0]
    );
#endif
    return 1;
}

static int __conformance_vector_index(lua_State* state)
{
    const float* vector = luaL_checkvector(state, 1);
    const char* name = luaL_checkstring(state, 2);

    if (strcmp(name, "Magnitude") == 0)
    {
        float square = vector[0] * vector[0] + vector[1] * vector[1] +
            vector[2] * vector[2];
#if LUA_VECTOR_SIZE == 4
        square += vector[3] * vector[3];
#endif
        lua_pushnumber(state, sqrtf(square));
        return 1;
    }
    if (strcmp(name, "Unit") == 0)
    {
        float square = vector[0] * vector[0] + vector[1] * vector[1] +
            vector[2] * vector[2];
        float inverse;
#if LUA_VECTOR_SIZE == 4
        square += vector[3] * vector[3];
#endif
        inverse = 1.0f / sqrtf(square);
#if LUA_VECTOR_SIZE == 4
        lua_pushvector(
            state,
            vector[0] * inverse,
            vector[1] * inverse,
            vector[2] * inverse,
            vector[3] * inverse
        );
#else
        lua_pushvector(
            state,
            vector[0] * inverse,
            vector[1] * inverse,
            vector[2] * inverse
        );
#endif
        return 1;
    }
    if (strcmp(name, "Dot") == 0)
    {
        lua_pushcfunction(state, __conformance_vector_dot, "Dot");
        return 1;
    }

    luaL_error(state, "%s is not a valid member of vector", name);
    return 0;
}

static int __conformance_vector_namecall(lua_State* state)
{
    const char* name = lua_namecallatom(state, NULL);
    if (name != NULL)
    {
        if (strcmp(name, "Dot") == 0)
            return __conformance_vector_dot(state);
        if (strcmp(name, "Cross") == 0)
            return __conformance_vector_cross(state);
    }
    luaL_error(state, "%s is not a valid method of vector", luaL_checkstring(state, 1));
    return 0;
}

static void __conformance_setup_vector(lua_State* state)
{
#if LUA_VECTOR_SIZE == 4
    lua_pushvector(state, 0.0f, 0.0f, 0.0f, 0.0f);
#else
    lua_pushvector(state, 0.0f, 0.0f, 0.0f);
#endif
    luaL_newmetatable(state, "vector");
    lua_pushcfunction(state, __conformance_vector_index, NULL);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, __conformance_vector_namecall, NULL);
    lua_setfield(state, -2, "__namecall");
    lua_setreadonly(state, -1, 1);
    lua_setmetatable(state, -2);
    lua_pop(state, 1);
}

enum
{
    CONFORMANCE_INT64_TAG = 1
};

static int64_t __conformance_get_int64(lua_State* state, int index)
{
    int64_t* value =
        (int64_t*)lua_touserdatatagged(state, index, CONFORMANCE_INT64_TAG);
    if (value != NULL)
        return *value;
    if (lua_isnumber(state, index))
        return (int64_t)lua_tointeger(state, index);
    luaL_typeerror(state, index, "int64");
    return 0;
}

static void __conformance_push_int64(lua_State* state, int64_t value)
{
    int64_t* result =
        (int64_t*)lua_newuserdatatagged(state, sizeof(int64_t), CONFORMANCE_INT64_TAG);
    luaL_getmetatable(state, "int64");
    lua_setmetatable(state, -2);
    *result = value;
}

static int __conformance_int64_index(lua_State* state)
{
    int64_t* value =
        (int64_t*)luaL_checkudatatagged(state, 1, CONFORMANCE_INT64_TAG);
    const char* name = luaL_checkstring(state, 2);
    if (strcmp(name, "value") == 0)
    {
        lua_pushnumber(state, (double)*value);
        return 1;
    }
    luaL_error(state, "unknown field %s", name);
    return 0;
}

static int __conformance_int64_newindex(lua_State* state)
{
    int64_t* value =
        (int64_t*)luaL_checkudatatagged(state, 1, CONFORMANCE_INT64_TAG);
    const char* name = luaL_checkstring(state, 2);
    if (strcmp(name, "value") == 0)
    {
        *value = (int64_t)luaL_checknumber(state, 3);
        return 0;
    }
    luaL_error(state, "unknown field %s", name);
    return 0;
}

static int __conformance_int64_eq(lua_State* state)
{
    lua_pushboolean(
        state, __conformance_get_int64(state, 1) == __conformance_get_int64(state, 2)
    );
    return 1;
}

static int __conformance_int64_lt(lua_State* state)
{
    lua_pushboolean(
        state, __conformance_get_int64(state, 1) < __conformance_get_int64(state, 2)
    );
    return 1;
}

static int __conformance_int64_le(lua_State* state)
{
    lua_pushboolean(
        state, __conformance_get_int64(state, 1) <= __conformance_get_int64(state, 2)
    );
    return 1;
}

static int __conformance_int64_add(lua_State* state)
{
    uint64_t left = (uint64_t)__conformance_get_int64(state, 1);
    uint64_t right = (uint64_t)__conformance_get_int64(state, 2);
    __conformance_push_int64(state, (int64_t)(left + right));
    return 1;
}

static int __conformance_int64_sub(lua_State* state)
{
    uint64_t left = (uint64_t)__conformance_get_int64(state, 1);
    uint64_t right = (uint64_t)__conformance_get_int64(state, 2);
    __conformance_push_int64(state, (int64_t)(left - right));
    return 1;
}

static int __conformance_int64_mul(lua_State* state)
{
    uint64_t left = (uint64_t)__conformance_get_int64(state, 1);
    uint64_t right = (uint64_t)__conformance_get_int64(state, 2);
    __conformance_push_int64(state, (int64_t)(left * right));
    return 1;
}

static int __conformance_int64_div(lua_State* state)
{
    __conformance_push_int64(
        state,
        __conformance_get_int64(state, 1) / __conformance_get_int64(state, 2)
    );
    return 1;
}

static int __conformance_int64_idiv(lua_State* state)
{
    double left = (double)__conformance_get_int64(state, 1);
    double right = (double)__conformance_get_int64(state, 2);
    __conformance_push_int64(state, (int64_t)floor(left / right));
    return 1;
}

static int __conformance_int64_mod(lua_State* state)
{
    __conformance_push_int64(
        state,
        __conformance_get_int64(state, 1) % __conformance_get_int64(state, 2)
    );
    return 1;
}

static int __conformance_int64_pow(lua_State* state)
{
    __conformance_push_int64(
        state,
        (int64_t)pow(
            (double)__conformance_get_int64(state, 1),
            (double)__conformance_get_int64(state, 2)
        )
    );
    return 1;
}

static int __conformance_int64_unm(lua_State* state)
{
    uint64_t value = (uint64_t)__conformance_get_int64(state, 1);
    __conformance_push_int64(state, (int64_t)(~value + UINT64_C(1)));
    return 1;
}

static int __conformance_int64_tostring(lua_State* state)
{
    char buffer[32];
    int length = snprintf(
        buffer, sizeof(buffer), "%" PRId64, __conformance_get_int64(state, 1)
    );
    lua_pushlstring(state, buffer, length > 0 ? (size_t)length : 0);
    return 1;
}

static int __conformance_int64_constructor(lua_State* state)
{
    __conformance_push_int64(state, (int64_t)luaL_checknumber(state, 1));
    return 1;
}

static void __conformance_setup_int64(lua_State* state)
{
    static const luaL_Reg __metamethods[] = {
        {"__index", __conformance_int64_index},
        {"__newindex", __conformance_int64_newindex},
        {"__eq", __conformance_int64_eq},
        {"__lt", __conformance_int64_lt},
        {"__le", __conformance_int64_le},
        {"__add", __conformance_int64_add},
        {"__sub", __conformance_int64_sub},
        {"__mul", __conformance_int64_mul},
        {"__div", __conformance_int64_div},
        {"__idiv", __conformance_int64_idiv},
        {"__mod", __conformance_int64_mod},
        {"__pow", __conformance_int64_pow},
        {"__unm", __conformance_int64_unm},
        {"__tostring", __conformance_int64_tostring},
        {NULL, NULL}
    };

    luaL_newmetatable(state, "int64");
    lua_pushliteral(state, "int64");
    lua_setfield(state, -2, "__type");
    luaL_register(state, NULL, __metamethods);
    lua_pop(state, 1);
    lua_pushcfunction(state, __conformance_int64_constructor, "int64");
    lua_setglobal(state, "int64");
}

static void* __conformance_limited_reallocate(
    void* context, void* pointer, size_t old_size, size_t new_size
)
{
    (void)context;
    (void)old_size;
    if (new_size == 0)
    {
        free(pointer);
        return NULL;
    }
    if (new_size > 8u * 1024u * 1024u)
        return NULL;
    return realloc(pointer, new_size);
}

static void* __conformance_blockable_reallocate(
    void* context, void* pointer, size_t old_size, size_t new_size
)
{
    (void)context;
    (void)old_size;
    if (new_size == 0)
    {
        free(pointer);
        return NULL;
    }
    if (!__conformance_allocations_allowed)
        return NULL;
    return realloc(pointer, new_size);
}

static int __conformance_set_block_allocations(lua_State* state)
{
    __conformance_allocations_allowed = !luaL_checkboolean(state, 1);
    return 0;
}

static int __conformance_c_error(lua_State* state)
{
    luaL_error(state, "%s", "oops");
    return 0;
}

static int __conformance_resume_error(lua_State* state)
{
    lua_State* coroutine = lua_tothread(state, 1);
    lua_xmove(state, coroutine, 1);
    lua_resumeerror(coroutine, state);
    return 0;
}

static void __conformance_debug_break(lua_State* state, lua_Debug* debug)
{
    (void)debug;
    __conformance_break_hits++;
    (void)lua_debugtrace(state);
    if ((__conformance_break_hits & 1) != 0)
        lua_break(state);
}

static void __conformance_debug_interrupt(lua_State* state, lua_Debug* debug)
{
    (void)state;
    if (debug != NULL)
        __conformance_interrupted_thread = (lua_State*)debug->userdata;
}

static int __conformance_set_breakpoint(lua_State* state)
{
    int line = luaL_checkinteger(state, 1);
    int enabled = luaL_optboolean(state, 2, 1);
    lua_Debug debug;
    memset(&debug, 0, sizeof(debug));
    if (!lua_getinfo(state, lua_stackdepth(state) - 1, "f", &debug))
    {
        luaL_errorL(state, "%s", "unable to find debugger target function");
        return 0;
    }
    lua_breakpoint(state, -1, line, enabled);
    return 0;
}

static int __conformance_single_yield(lua_State* state)
{
    lua_pushnumber(state, 2);
    return lua_yield(state, 1);
}

static int __conformance_single_yield_continuation(lua_State* state, int status)
{
    (void)status;
    lua_pushnumber(state, 4);
    return 1;
}

static int __conformance_multiple_yields(lua_State* state)
{
    int base;
    lua_settop(state, 1);
    base = luaL_checkinteger(state, 1);
    luaL_checkstack(state, 2, "cmultiyield");
    lua_pushinteger(state, 1);
    lua_pushinteger(state, base + 1);
    return lua_yield(state, 1);
}

static int __conformance_multiple_yields_continuation(lua_State* state, int status)
{
    int base = luaL_checkinteger(state, 1);
    int position = luaL_checkinteger(state, 2) + 1;
    (void)status;
    luaL_checkstack(state, 1, "cmultiyieldcont");
    lua_pushinteger(state, position);
    lua_replace(state, 2);
    lua_pushinteger(state, base + position);
    return position < 4 ? lua_yield(state, 1) : 1;
}

static int __conformance_nested_yield(lua_State* state)
{
    int context = luaL_checkinteger(state, lua_upvalueindex(1));
    lua_pushinteger(state, 100 + context);
    return lua_yield(state, 1);
}

static int __conformance_nested_yield_continuation(lua_State* state, int status)
{
    int context = luaL_checkinteger(state, lua_upvalueindex(1));
    (void)status;
    lua_pushinteger(state, 110 + context);
    return 1;
}

static int __conformance_nested_no_yield(lua_State* state)
{
    int context = luaL_checkinteger(state, lua_upvalueindex(1));
    lua_pushinteger(state, 105 + context);
    return 1;
}

static int __conformance_multiple_yields_nested(lua_State* state)
{
    int should_yield;
    lua_settop(state, 2);
    should_yield = luaL_checkboolean(state, 2);
    lua_pushinteger(state, 0);
    lua_pushnumber(state, 5);
    if (should_yield)
        lua_pushcclosurek(
            state,
            __conformance_nested_yield,
            NULL,
            1,
            __conformance_nested_yield_continuation
        );
    else
        lua_pushcclosurek(state, __conformance_nested_no_yield, NULL, 1, NULL);
    return luaL_callyieldable(state, 0, 1);
}

static int __conformance_multiple_yields_nested_continuation(
    lua_State* state, int status
)
{
    int step = luaL_checkinteger(state, 3);
    (void)status;
    luaL_checkstack(state, 1, "cnestedmultiyieldcont");
    lua_pushinteger(state, step + 1);
    lua_replace(state, 3);
    if (step == 0)
        return lua_yield(state, lua_gettop(state) - 3);
    if (step == 1)
    {
        lua_pushnumber(state, luaL_checkinteger(state, 1) + 200);
        return lua_yield(state, 1);
    }
    lua_pushnumber(state, luaL_checkinteger(state, 1) + 210);
    return 1;
}

static int __conformance_passthrough(lua_State* state)
{
    luaL_checkstack(state, 3, "cpass");
    lua_pushvalue(state, 1);
    lua_pushvalue(state, 2);
    lua_pushvalue(state, 3);
    return luaL_callyieldable(state, 2, 1);
}

static int __conformance_passthrough_continuation(lua_State* state, int status)
{
    (void)state;
    (void)status;
    return 1;
}

static int __conformance_passthrough_more(lua_State* state)
{
    luaL_checkstack(state, 3, "cpass");
    lua_pushvalue(state, 1);
    lua_pushvalue(state, 2);
    lua_pushvalue(state, 3);
    return luaL_callyieldable(state, 2, 10);
}

static int __conformance_passthrough_more_continuation(lua_State* state, int status)
{
    int index;
    (void)status;
    for (index = 0; index < 9; ++index)
        lua_pop(state, 1);
    return 1;
}

static int __conformance_passthrough_reuse(lua_State* state)
{
    return luaL_callyieldable(state, 2, 1);
}

static int __conformance_passthrough_reuse_continuation(lua_State* state, int status)
{
    (void)state;
    (void)status;
    return 1;
}

static int __conformance_passthrough_variadic(lua_State* state)
{
    luaL_checkany(state, 1);
    return luaL_callyieldable(state, lua_gettop(state) - 1, LUA_MULTRET);
}

static int __conformance_passthrough_variadic_continuation(
    lua_State* state, int status
)
{
    (void)status;
    return lua_gettop(state);
}

static int __conformance_passthrough_state(lua_State* state)
{
    int arguments;
    luaL_checkany(state, 1);
    arguments = lua_gettop(state) - 1;
    lua_pushnumber(state, 42);
    lua_insert(state, 1);
    return luaL_callyieldable(state, arguments, LUA_MULTRET);
}

static int __conformance_passthrough_state_continuation(lua_State* state, int status)
{
    (void)luaL_checkinteger(state, 1);
    (void)status;
    return lua_gettop(state) - 1;
}

static int __conformance_pcall_then_call(lua_State* state)
{
    luaL_checkany(state, 1);
    luaL_checkany(state, 2);
    luaL_checkstack(state, 3, "pcallThenCall");
    lua_pushinteger(state, 0);
    lua_pushinteger(state, 0);
    lua_pushvalue(state, 1);
    return luaL_pcallyieldable(state, 0, 1, 0);
}

static int __conformance_pcall_then_call_continuation(lua_State* state, int status)
{
    int protected_variant = lua_tointeger(state, lua_upvalueindex(1));
    int step = luaL_checkinteger(state, 3);
    luaL_checkstack(state, 1, "pcallThenCallContinuation");

    if (step == 0)
    {
        if (status != LUA_OK)
        {
            lua_pushinteger(state, -1);
            lua_replace(state, 4);
        }
        else
            lua_replace(state, 4);
        lua_pushinteger(state, 1);
        lua_replace(state, 3);
        lua_pushvalue(state, 2);
        return protected_variant ?
            luaL_pcallyieldable(state, 0, LUA_MULTRET, 0) :
            luaL_callyieldable(state, 0, LUA_MULTRET);
    }

    {
        int multiplier = luaL_checkinteger(state, 4);
        int value = status == LUA_OK ? lua_tointeger(state, -1) : -1;
        lua_pushinteger(state, multiplier * value);
        return 1;
    }
}

static void __conformance_setup_cyield(lua_State* state)
{
    lua_pushcclosurek(
        state,
        __conformance_single_yield,
        "singleYield",
        0,
        __conformance_single_yield_continuation
    );
    lua_setglobal(state, "singleYield");
    lua_pushcclosurek(
        state,
        __conformance_multiple_yields,
        "multipleYields",
        0,
        __conformance_multiple_yields_continuation
    );
    lua_setglobal(state, "multipleYields");
    lua_pushcclosurek(
        state,
        __conformance_multiple_yields_nested,
        "multipleYieldsWithNestedCall",
        0,
        __conformance_multiple_yields_nested_continuation
    );
    lua_setglobal(state, "multipleYieldsWithNestedCall");
    lua_pushcclosurek(
        state,
        __conformance_passthrough,
        "passthroughCall",
        0,
        __conformance_passthrough_continuation
    );
    lua_setglobal(state, "passthroughCall");
    lua_pushcclosurek(
        state,
        __conformance_passthrough_more,
        "passthroughCallMoreResults",
        0,
        __conformance_passthrough_more_continuation
    );
    lua_setglobal(state, "passthroughCallMoreResults");
    lua_pushcclosurek(
        state,
        __conformance_passthrough_reuse,
        "passthroughCallArgReuse",
        0,
        __conformance_passthrough_reuse_continuation
    );
    lua_setglobal(state, "passthroughCallArgReuse");
    lua_pushcclosurek(
        state,
        __conformance_passthrough_variadic,
        "passthroughCallVaradic",
        0,
        __conformance_passthrough_variadic_continuation
    );
    lua_setglobal(state, "passthroughCallVaradic");
    lua_pushcclosurek(
        state,
        __conformance_passthrough_state,
        "passthroughCallWithState",
        0,
        __conformance_passthrough_state_continuation
    );
    lua_setglobal(state, "passthroughCallWithState");
    lua_pushinteger(state, 0);
    lua_pushcclosurek(
        state,
        __conformance_pcall_then_call,
        "pcallThenCall",
        1,
        __conformance_pcall_then_call_continuation
    );
    lua_setglobal(state, "pcallThenCall");
    lua_pushinteger(state, 1);
    lua_pushcclosurek(
        state,
        __conformance_pcall_then_call,
        "pcallThenPcall",
        1,
        __conformance_pcall_then_call_continuation
    );
    lua_setglobal(state, "pcallThenPcall");
}

static void __conformance_push_runtime_type(lua_State* state, int index, unsigned int depth)
{
    int type = lua_type(state, index);
    int absolute = lua_absindex(state, index);

    if (type != LUA_TTABLE || depth >= 16)
    {
        lua_pushstring(state, luaL_typename(state, absolute));
        return;
    }

    lua_newtable(state);
    {
        int result = lua_absindex(state, -1);
        lua_pushnil(state);
        while (lua_next(state, absolute) != 0)
        {
            if (lua_type(state, -2) == LUA_TSTRING &&
                strcmp(lua_tostring(state, -2), "_G") != 0)
            {
                lua_pushvalue(state, -2);
                __conformance_push_runtime_type(state, -2, depth + 1);
                lua_rawset(state, result);
            }
            lua_pop(state, 1);
        }
    }
}

static void __conformance_setup_types(lua_State* state)
{
    __conformance_push_runtime_type(state, LUA_GLOBALSINDEX, 0);
    lua_setglobal(state, "RTTI");
}

typedef struct conformance_vec2_t
{
    float x;
    float y;
} conformance_vec2_t;

typedef struct conformance_vertex_t
{
    float position[3];
    float normal[3];
    float uv[2];
} conformance_vertex_t;

enum
{
    CONFORMANCE_VEC2_TAG = 12,
    CONFORMANCE_VERTEX_TAG = 13,
    CONFORMANCE_SLOT_X = 1,
    CONFORMANCE_SLOT_Y,
    CONFORMANCE_SLOT_MAGNITUDE,
    CONFORMANCE_SLOT_UNIT,
    CONFORMANCE_SLOT_DOT,
    CONFORMANCE_SLOT_MIN,
    CONFORMANCE_SLOT_CLONE,
    CONFORMANCE_SLOT_REENTER,
    CONFORMANCE_SLOT_POSITION,
    CONFORMANCE_SLOT_NORMAL,
    CONFORMANCE_SLOT_UV,
    CONFORMANCE_SLOT_SIZEOF
};

static conformance_vec2_t* __conformance_push_vec2(lua_State* state)
{
    conformance_vec2_t* result = (conformance_vec2_t*)lua_newuserdatatagged(
        state, sizeof(conformance_vec2_t), CONFORMANCE_VEC2_TAG
    );
    lua_getuserdatametatable(state, CONFORMANCE_VEC2_TAG);
    lua_setmetatable(state, -2);
    return result;
}

static conformance_vec2_t* __conformance_get_vec2(lua_State* state, int index)
{
    return (conformance_vec2_t*)luaL_checkudatatagged(
        state, index, CONFORMANCE_VEC2_TAG
    );
}

static conformance_vertex_t* __conformance_push_vertex(lua_State* state)
{
    conformance_vertex_t* result = (conformance_vertex_t*)lua_newuserdatatagged(
        state, sizeof(conformance_vertex_t), CONFORMANCE_VERTEX_TAG
    );
    lua_getuserdatametatable(state, CONFORMANCE_VERTEX_TAG);
    lua_setmetatable(state, -2);
    return result;
}

static conformance_vertex_t* __conformance_get_vertex(lua_State* state, int index)
{
    conformance_vertex_t* result = (conformance_vertex_t*)lua_touserdatatagged(
        state, index, CONFORMANCE_VERTEX_TAG
    );
    if (result == NULL)
    {
        luaL_typeerror(state, index, "vertex");
        return NULL;
    }
    return result;
}

static void __conformance_push_vector3(lua_State* state, float x, float y, float z)
{
#if LUA_VECTOR_SIZE == 4
    lua_pushvector(state, x, y, z, 0.0f);
#else
    lua_pushvector(state, x, y, z);
#endif
}

static int __conformance_vec2_constructor(lua_State* state)
{
    conformance_vec2_t* result;
    double x = luaL_checknumber(state, 1);
    double y = luaL_checknumber(state, 2);
    result = __conformance_push_vec2(state);
    result->x = (float)x;
    result->y = (float)y;
    return 1;
}

static int __conformance_vec2_dot(lua_State* state, conformance_vec2_t* self)
{
    conformance_vec2_t* other = __conformance_get_vec2(state, 2);
    lua_pushnumber(state, self->x * other->x + self->y * other->y);
    return 1;
}

static int __conformance_vec2_min(lua_State* state, conformance_vec2_t* self)
{
    conformance_vec2_t* other = __conformance_get_vec2(state, 2);
    conformance_vec2_t* result = __conformance_push_vec2(state);
    result->x = self->x < other->x ? self->x : other->x;
    result->y = self->y < other->y ? self->y : other->y;
    return 1;
}

static int __conformance_vec2_clone(lua_State* state, conformance_vec2_t* self)
{
    conformance_vec2_t* result = __conformance_push_vec2(state);
    *result = *self;
    return 1;
}

static int __conformance_vec2_reenter(lua_State* state, conformance_vec2_t* self)
{
    lua_getglobal(state, "reenterCallback");
    if (!lua_isfunction(state, -1))
    {
        luaL_error(state, "%s", "reenterCallback must be a function");
        return 0;
    }
    (void)lua_pcall(state, 0, 0, 0);
    lua_pushnumber(state, self->x + self->y);
    return 1;
}

static int __conformance_vec2_index(lua_State* state)
{
    conformance_vec2_t* value = __conformance_get_vec2(state, 1);
    const char* name = luaL_checkstring(state, 2);
    if (strcmp(name, "X") == 0)
        lua_pushnumber(state, value->x);
    else if (strcmp(name, "Y") == 0)
        lua_pushnumber(state, value->y);
    else if (strcmp(name, "Magnitude") == 0)
        lua_pushnumber(state, sqrtf(value->x * value->x + value->y * value->y));
    else if (strcmp(name, "Unit") == 0)
    {
        float inverse =
            1.0f / sqrtf(value->x * value->x + value->y * value->y);
        conformance_vec2_t* result = __conformance_push_vec2(state);
        result->x = value->x * inverse;
        result->y = value->y * inverse;
    }
    else if (strcmp(name, "sizeof") == 0)
        lua_pushnumber(state, sizeof(conformance_vec2_t));
    else
    {
        luaL_error(state, "%s is not a valid member of vec2", name);
        return 0;
    }
    return 1;
}

static int __conformance_vec2_newindex(lua_State* state)
{
    conformance_vec2_t* value = __conformance_get_vec2(state, 1);
    const char* name = luaL_checkstring(state, 2);
    double replacement = luaL_checknumber(state, 3);
    if (strcmp(name, "X") == 0)
        value->x = (float)replacement;
    else if (strcmp(name, "Y") == 0)
        value->y = (float)replacement;
    else
        luaL_error(state, "%s is not a writable member of vec2", name);
    return 0;
}

static int __conformance_vec2_namecall(lua_State* state)
{
    const char* name = lua_namecallatom(state, NULL);
    conformance_vec2_t* self = __conformance_get_vec2(state, 1);
    if (name != NULL && strcmp(name, "Dot") == 0)
        return __conformance_vec2_dot(state, self);
    if (name != NULL && strcmp(name, "Min") == 0)
        return __conformance_vec2_min(state, self);
    if (name != NULL && strcmp(name, "Clone") == 0)
        return __conformance_vec2_clone(state, self);
    if (name != NULL && strcmp(name, "Reenter") == 0)
        return __conformance_vec2_reenter(state, self);
    luaL_error(state, "%s is not a valid method of vec2", name != NULL ? name : "");
    return 0;
}

static int __conformance_vec2_add(lua_State* state)
{
    conformance_vec2_t* left = __conformance_get_vec2(state, 1);
    conformance_vec2_t* right = __conformance_get_vec2(state, 2);
    conformance_vec2_t* result = __conformance_push_vec2(state);
    result->x = left->x + right->x;
    result->y = left->y + right->y;
    return 1;
}

static int __conformance_vec2_sub(lua_State* state)
{
    conformance_vec2_t* left = __conformance_get_vec2(state, 1);
    conformance_vec2_t* right = __conformance_get_vec2(state, 2);
    conformance_vec2_t* result = __conformance_push_vec2(state);
    result->x = left->x - right->x;
    result->y = left->y - right->y;
    return 1;
}

static int __conformance_vec2_mul(lua_State* state)
{
    conformance_vec2_t* left = __conformance_get_vec2(state, 1);
    conformance_vec2_t* right = __conformance_get_vec2(state, 2);
    conformance_vec2_t* result = __conformance_push_vec2(state);
    result->x = left->x * right->x;
    result->y = left->y * right->y;
    return 1;
}

static int __conformance_vec2_div(lua_State* state)
{
    conformance_vec2_t* left = __conformance_get_vec2(state, 1);
    conformance_vec2_t* right = __conformance_get_vec2(state, 2);
    conformance_vec2_t* result = __conformance_push_vec2(state);
    result->x = left->x / right->x;
    result->y = left->y / right->y;
    return 1;
}

static int __conformance_vec2_unm(lua_State* state)
{
    conformance_vec2_t* value = __conformance_get_vec2(state, 1);
    conformance_vec2_t* result = __conformance_push_vec2(state);
    result->x = -value->x;
    result->y = -value->y;
    return 1;
}

static int __conformance_vertex_constructor(lua_State* state)
{
    const float* position = luaL_checkvector(state, 1);
    const float* normal = luaL_checkvector(state, 2);
    conformance_vec2_t* uv = __conformance_get_vec2(state, 3);
    conformance_vertex_t* result = __conformance_push_vertex(state);
    memcpy(result->position, position, sizeof(result->position));
    memcpy(result->normal, normal, sizeof(result->normal));
    result->uv[0] = uv->x;
    result->uv[1] = uv->y;
    return 1;
}

static int __conformance_vertex_clone(lua_State* state, conformance_vertex_t* self)
{
    conformance_vertex_t* result = __conformance_push_vertex(state);
    *result = *self;
    return 1;
}

static int __conformance_vertex_index(lua_State* state)
{
    conformance_vertex_t* value = __conformance_get_vertex(state, 1);
    const char* name = luaL_checkstring(state, 2);
    if (strcmp(name, "pos") == 0)
        __conformance_push_vector3(
            state, value->position[0], value->position[1], value->position[2]
        );
    else if (strcmp(name, "normal") == 0)
        __conformance_push_vector3(
            state, value->normal[0], value->normal[1], value->normal[2]
        );
    else if (strcmp(name, "uv") == 0)
    {
        conformance_vec2_t* result = __conformance_push_vec2(state);
        result->x = value->uv[0];
        result->y = value->uv[1];
    }
    else if (strcmp(name, "sizeof") == 0)
        lua_pushnumber(state, sizeof(conformance_vertex_t));
    else
    {
        luaL_error(state, "%s is not a valid member of vertex", name);
        return 0;
    }
    return 1;
}

static int __conformance_vertex_newindex(lua_State* state)
{
    conformance_vertex_t* value = __conformance_get_vertex(state, 1);
    const char* name = luaL_checkstring(state, 2);
    if (strcmp(name, "pos") == 0)
    {
        const float* replacement = luaL_checkvector(state, 3);
        memcpy(value->position, replacement, sizeof(value->position));
    }
    else if (strcmp(name, "normal") == 0)
    {
        const float* replacement = luaL_checkvector(state, 3);
        memcpy(value->normal, replacement, sizeof(value->normal));
    }
    else if (strcmp(name, "uv") == 0)
    {
        conformance_vec2_t* replacement = __conformance_get_vec2(state, 3);
        value->uv[0] = replacement->x;
        value->uv[1] = replacement->y;
    }
    else
        luaL_error(state, "%s is not a writable member of vertex", name);
    return 0;
}

static int __conformance_vertex_namecall(lua_State* state)
{
    const char* name = lua_namecallatom(state, NULL);
    conformance_vertex_t* self = __conformance_get_vertex(state, 1);
    if (name != NULL && strcmp(name, "Clone") == 0)
        return __conformance_vertex_clone(state, self);
    luaL_error(state, "%s is not a valid method of vertex", name != NULL ? name : "");
    return 0;
}

static int16_t __conformance_user_atom(lua_State* state, const char* name, size_t length)
{
    static const char* const __names[] = {
        NULL, "X", "Y", "Magnitude", "Unit", "Dot", "Min", "Clone", "Reenter",
        "pos", "normal", "uv", "sizeof"
    };
    size_t index;
    (void)state;
    for (index = 1; index < sizeof(__names) / sizeof(__names[0]); ++index)
        if (strlen(__names[index]) == length && memcmp(__names[index], name, length) == 0)
            return (int16_t)index;
    return -1;
}

static void __conformance_update_direct_slot(int atom, uint16_t* cached_slot)
{
    if (atom >= CONFORMANCE_SLOT_X && atom <= CONFORMANCE_SLOT_SIZEOF)
        *cached_slot = (uint16_t)atom;
}

static void __conformance_vec2_direct_index(
    lua_State* state, void* data, int atom, uint16_t* cached_slot, int tag
)
{
    conformance_vec2_t* self = (conformance_vec2_t*)data;
    (void)tag;
    if (*cached_slot == 0)
        __conformance_update_direct_slot(atom, cached_slot);
    switch (*cached_slot)
    {
    case CONFORMANCE_SLOT_X:
        lua_pushnumber(state, self->x);
        break;
    case CONFORMANCE_SLOT_Y:
        lua_pushnumber(state, self->y);
        break;
    case CONFORMANCE_SLOT_MAGNITUDE:
        lua_pushnumber(state, sqrtf(self->x * self->x + self->y * self->y));
        break;
    case CONFORMANCE_SLOT_UNIT:
    {
        float inverse = 1.0f / sqrtf(self->x * self->x + self->y * self->y);
        conformance_vec2_t* result = __conformance_push_vec2(state);
        result->x = self->x * inverse;
        result->y = self->y * inverse;
        break;
    }
    case CONFORMANCE_SLOT_SIZEOF:
        lua_pushnumber(state, sizeof(conformance_vec2_t));
        break;
    default:
        luaL_error(state, "%s is not a valid member of vec2", luaL_checkstring(state, 2));
    }
}

static void __conformance_vec2_direct_newindex(
    lua_State* state, void* data, int atom, uint16_t* cached_slot, int tag
)
{
    conformance_vec2_t* self = (conformance_vec2_t*)data;
    (void)tag;
    if (*cached_slot == 0)
        __conformance_update_direct_slot(atom, cached_slot);
    if (*cached_slot == CONFORMANCE_SLOT_X)
        self->x = (float)luaL_checknumber(state, 3);
    else if (*cached_slot == CONFORMANCE_SLOT_Y)
        self->y = (float)luaL_checknumber(state, 3);
    else
        luaL_error(state, "%s is not a writable member of vec2", luaL_checkstring(state, 2));
}

static int __conformance_vec2_direct_namecall(
    lua_State* state, void* data, int atom, uint16_t* cached_slot, int tag
)
{
    conformance_vec2_t* self = (conformance_vec2_t*)data;
    (void)tag;
    if (*cached_slot == 0)
        __conformance_update_direct_slot(atom, cached_slot);
    if (*cached_slot == CONFORMANCE_SLOT_DOT)
        return __conformance_vec2_dot(state, self);
    if (*cached_slot == CONFORMANCE_SLOT_MIN)
        return __conformance_vec2_min(state, self);
    if (*cached_slot == CONFORMANCE_SLOT_CLONE)
        return __conformance_vec2_clone(state, self);
    if (*cached_slot == CONFORMANCE_SLOT_REENTER)
        return __conformance_vec2_reenter(state, self);
    luaL_error(state, "%s is not a valid method of vec2", lua_namecallatom(state, NULL));
    return 0;
}

static void __conformance_vertex_direct_index(
    lua_State* state, void* data, int atom, uint16_t* cached_slot, int tag
)
{
    conformance_vertex_t* self = (conformance_vertex_t*)data;
    (void)tag;
    if (*cached_slot == 0)
        __conformance_update_direct_slot(atom, cached_slot);
    if (*cached_slot == CONFORMANCE_SLOT_POSITION)
        __conformance_push_vector3(
            state, self->position[0], self->position[1], self->position[2]
        );
    else if (*cached_slot == CONFORMANCE_SLOT_NORMAL)
        __conformance_push_vector3(
            state, self->normal[0], self->normal[1], self->normal[2]
        );
    else if (*cached_slot == CONFORMANCE_SLOT_UV)
    {
        conformance_vec2_t* result = __conformance_push_vec2(state);
        result->x = self->uv[0];
        result->y = self->uv[1];
    }
    else if (*cached_slot == CONFORMANCE_SLOT_SIZEOF)
        lua_pushnumber(state, sizeof(conformance_vertex_t));
    else
        luaL_error(state, "%s is not a valid member of vertex", luaL_checkstring(state, 2));
}

static void __conformance_vertex_direct_newindex(
    lua_State* state, void* data, int atom, uint16_t* cached_slot, int tag
)
{
    conformance_vertex_t* self = (conformance_vertex_t*)data;
    (void)tag;
    if (*cached_slot == 0)
        __conformance_update_direct_slot(atom, cached_slot);
    if (*cached_slot == CONFORMANCE_SLOT_POSITION)
    {
        const float* value = luaL_checkvector(state, 3);
        memcpy(self->position, value, sizeof(self->position));
    }
    else if (*cached_slot == CONFORMANCE_SLOT_NORMAL)
    {
        const float* value = luaL_checkvector(state, 3);
        memcpy(self->normal, value, sizeof(self->normal));
    }
    else if (*cached_slot == CONFORMANCE_SLOT_UV)
    {
        conformance_vec2_t* value = __conformance_get_vec2(state, 3);
        self->uv[0] = value->x;
        self->uv[1] = value->y;
    }
    else
        luaL_error(state, "%s is not a writable member of vertex", luaL_checkstring(state, 2));
}

static int __conformance_vertex_direct_namecall(
    lua_State* state, void* data, int atom, uint16_t* cached_slot, int tag
)
{
    conformance_vertex_t* self = (conformance_vertex_t*)data;
    (void)tag;
    if (*cached_slot == 0)
        __conformance_update_direct_slot(atom, cached_slot);
    if (*cached_slot == CONFORMANCE_SLOT_CLONE)
        return __conformance_vertex_clone(state, self);
    luaL_error(state, "%s is not a valid method of vertex", lua_namecallatom(state, NULL));
    return 0;
}

static void __conformance_setup_direct_userdata(lua_State* state)
{
    luaL_newmetatable(state, "vec2");
    lua_pushvalue(state, -1);
    lua_setuserdatametatable(state, CONFORMANCE_VEC2_TAG);
    lua_pushliteral(state, "vec2");
    lua_setfield(state, -2, "__type");
    lua_pushcfunction(state, __conformance_vec2_index, NULL);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, __conformance_vec2_newindex, NULL);
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, __conformance_vec2_namecall, NULL);
    lua_setfield(state, -2, "__namecall");
    lua_pushcfunction(state, __conformance_vec2_add, NULL);
    lua_setfield(state, -2, "__add");
    lua_pushcfunction(state, __conformance_vec2_sub, NULL);
    lua_setfield(state, -2, "__sub");
    lua_pushcfunction(state, __conformance_vec2_mul, NULL);
    lua_setfield(state, -2, "__mul");
    lua_pushcfunction(state, __conformance_vec2_div, NULL);
    lua_setfield(state, -2, "__div");
    lua_pushcfunction(state, __conformance_vec2_unm, NULL);
    lua_setfield(state, -2, "__unm");
    lua_setreadonly(state, -1, 1);
    lua_pop(state, 1);
    lua_pushcfunction(state, __conformance_vec2_constructor, "vec2");
    lua_setglobal(state, "vec2");

    luaL_newmetatable(state, "vertex");
    lua_pushvalue(state, -1);
    lua_setuserdatametatable(state, CONFORMANCE_VERTEX_TAG);
    lua_pushliteral(state, "vertex");
    lua_setfield(state, -2, "__type");
    lua_pushcfunction(state, __conformance_vertex_index, NULL);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, __conformance_vertex_newindex, NULL);
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, __conformance_vertex_namecall, NULL);
    lua_setfield(state, -2, "__namecall");
    lua_setreadonly(state, -1, 1);
    lua_pop(state, 1);
    lua_pushcfunction(state, __conformance_vertex_constructor, "vertex");
    lua_setglobal(state, "vertex");

    lua_callbacks(state)->useratom = __conformance_user_atom;
    (void)lua_registeruserdatadirectaccess(
        state,
        CONFORMANCE_VEC2_TAG,
        __conformance_vec2_direct_index,
        __conformance_vec2_direct_newindex,
        __conformance_vec2_direct_namecall
    );
    (void)lua_registeruserdatadirectaccess(
        state,
        CONFORMANCE_VERTEX_TAG,
        __conformance_vertex_direct_index,
        __conformance_vertex_direct_newindex,
        __conformance_vertex_direct_namecall
    );
}

static char* __read_file(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    long length;
    char* result;
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return NULL;
    }
    result = (char*)malloc((size_t)length + 1);
    if (result == NULL)
    {
        fclose(file);
        return NULL;
    }
    if (fread(result, 1, (size_t)length, file) != (size_t)length)
    {
        free(result);
        fclose(file);
        return NULL;
    }
    fclose(file);
    result[length] = '\0';
    *size = (size_t)length;
    return result;
}

static void __dump_proto(const proto_t* proto, unsigned int depth)
{
    int index;
    unsigned int indent;
    fprintf(
        stderr,
        "proto %s params=%d upvalues=%d stack=%d\n",
        proto->debugname != NULL ? getstr(proto->debugname) : "(main)",
        proto->numparams,
        proto->nups,
        proto->maxstacksize
    );
    for (index = 0; index < proto->sizecode;)
    {
        Instruction instruction = proto->code[index];
        luauc_opcode_t opcode = (luauc_opcode_t)LUAU_INSN_OP(instruction);
        for (indent = 0; indent < depth; ++indent)
            fputs("  ", stderr);
        fprintf(
            stderr,
            "%4d op=%3d a=%3d b=%3d c=%3d d=%6d\n",
            index,
            opcode,
            LUAU_INSN_A(instruction),
            LUAU_INSN_B(instruction),
            LUAU_INSN_C(instruction),
            LUAU_INSN_D(instruction)
        );
        index += (int)__luauc_get_op_length(opcode);
    }
    for (index = 0; index < proto->sizep; ++index)
        __dump_proto(proto->p[index], depth + 1);
}

int main(int argc, char** argv)
{
    static const luaL_Reg __functions[] = {
        {"collectgarbage", __conformance_collectgarbage},
        {"getcoverage", __conformance_getcoverage},
        {"loadstring", __conformance_loadstring},
        {"makelud", __conformance_makelud},
        {"breakpoint", __conformance_set_breakpoint},
        {"print", __conformance_silent_print},
        {"setblockallocations", __conformance_set_block_allocations},
        {"is_native", __conformance_is_native},
        {"is_native_if_supported", __conformance_is_native_if_supported},
        {NULL, NULL}
    };
    lua_CompileOptions options;
    static const char* __userdata_types[] = {
        "vec2", "color", "mat3", "vertex", NULL
    };
    lua_State* state;
    char* source;
    char* bytecode;
    size_t source_size;
    size_t bytecode_size = 0;
    int status;
    const char* basename;
    char chunkname[512];

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s conformance.luau\n", argv[0]);
        return 2;
    }
    basename = strrchr(argv[1], '/');
    basename = basename != NULL ? basename + 1 : argv[1];
    if (strcmp(basename, "native.luau") == 0 ||
        strcmp(basename, "native_types.luau") == 0 ||
        strcmp(basename, "integers_regspill.luau") == 0)
    {
        fprintf(stderr, "skipped: %s requires excluded CodeGen/JIT support\n", basename);
        return 77;
    }

    FFlag_LuauYieldIter2 = strcmp(basename, "iter.luau") == 0;
    FFlag_LuauCustomYieldablePcalls = strcmp(basename, "cyield.luau") == 0;
    FFlag_LuauIntegerLibrary =
        strcmp(basename, "integers.luau") == 0 ||
        strcmp(basename, "integers_regspill.luau") == 0;
    FFlag_DebugLuauUserDefinedClassesRuntime = strcmp(basename, "classes.luau") == 0;
    FFlag_LuauUdataDirectAccess6 = strcmp(basename, "udata_direct.luau") == 0;
    DFFlag_LuauGcTableStepFix = strcmp(basename, "gc.luau") == 0;
    __conformance_allocations_allowed = 1;

    source = __read_file(argv[1], &source_size);
    if (source == NULL)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    if (getenv("LUAUC_TRACE_ASSERT") != NULL)
    {
        static const char __prefix[] =
            "local __assert_count=0\n"
            "local __old_assert=assert\n"
            "assert=function(v,...)\n"
            " __assert_count += 1\n"
            " if not v then error(\"assertion #\"..__assert_count..\" at line \"..tostring(debug.info(2,\"l\")), 2) end\n"
            " return __old_assert(v,...)\n"
            "end\n";
        size_t combined_size = sizeof(__prefix) - 1 + source_size;
        char* combined = (char*)malloc(combined_size + 1);
        if (combined == NULL)
        {
            free(source);
            return 2;
        }
        memcpy(combined, __prefix, sizeof(__prefix) - 1);
        memcpy(combined + sizeof(__prefix) - 1, source, source_size + 1);
        free(source);
        source = combined;
        source_size = combined_size;
    }
    memset(&options, 0, sizeof(options));
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.typeInfoLevel = 1;
    if (strcmp(basename, "coverage.luau") == 0)
        options.coverageLevel = 2;
    if (strcmp(basename, "debugger.luau") == 0)
        options.debugLevel = 2;
    if (strcmp(basename, "ndebug_upvalues.luau") == 0)
    {
        options.optimizationLevel = 0;
        options.debugLevel = 0;
    }
    if (strcmp(basename, "native_userdata.luau") == 0)
        options.userdataTypes = __userdata_types;
    bytecode = luau_compile(source, source_size, &options, &bytecode_size);
    free(source);
    if (bytecode == NULL)
    {
        fprintf(stderr, "compiler allocation failure\n");
        return 2;
    }
    if (bytecode_size == 0 || (unsigned char)bytecode[0] == 0)
    {
        fprintf(stderr, "%.*s\n", (int)(bytecode_size - 1), bytecode + 1);
        free(bytecode);
        return 1;
    }

    if (strcmp(basename, "pcall.luau") == 0)
        state = lua_newstate(__conformance_limited_reallocate, NULL);
    else if (strcmp(basename, "gc.luau") == 0)
        state = lua_newstate(__conformance_blockable_reallocate, NULL);
    else
        state = luaL_newstate();
    if (state == NULL)
    {
        free(bytecode);
        return 2;
    }
    luaL_openlibs(state);
    if (strcmp(basename, "debugger.luau") == 0)
    {
        lua_Callbacks* callbacks = lua_callbacks(state);
        __conformance_break_hits = 0;
        __conformance_interrupted_thread = NULL;
        callbacks->debugbreak = __conformance_debug_break;
        callbacks->debuginterrupt = __conformance_debug_interrupt;
    }
    lua_pushvalue(state, LUA_GLOBALSINDEX);
    luaL_register(state, NULL, __functions);
    lua_pop(state, 1);
    lua_pushcclosurek(
        state,
        __conformance_c_yielding_iterator,
        "cYieldingIterator",
        0,
        __conformance_c_yielding_iterator_continuation
    );
    lua_setglobal(state, "cYieldingIterator");
    if (strcmp(basename, "pcall.luau") == 0)
    {
        lua_pushcfunction(state, __conformance_c_error, "cxxthrow");
        lua_setglobal(state, "cxxthrow");
        lua_pushcfunction(state, __conformance_resume_error, "resumeerror");
        lua_setglobal(state, "resumeerror");
    }
    if (strcmp(basename, "cyield.luau") == 0)
        __conformance_setup_cyield(state);
    if (strcmp(basename, "types.luau") == 0)
        __conformance_setup_types(state);
    if (strcmp(basename, "udata_direct.luau") == 0)
        __conformance_setup_direct_userdata(state);
    if (strcmp(basename, "native_userdata.luau") == 0)
        __conformance_setup_direct_userdata(state);
    if (strcmp(basename, "vector.luau") == 0 ||
        strcmp(basename, "native_types.luau") == 0 ||
        strcmp(basename, "native_userdata.luau") == 0 ||
        strcmp(basename, "udata_direct.luau") == 0)
        __conformance_setup_vector(state);
    if (strcmp(basename, "userdata.luau") == 0)
        __conformance_setup_int64(state);
    luaL_sandbox(state);
    luaL_sandboxthread(state);
    lua_pushvalue(state, LUA_GLOBALSINDEX);
    lua_setfield(state, -1, "_G");

    if (snprintf(chunkname, sizeof(chunkname), "=%s", basename) < 0)
    {
        free(bytecode);
        lua_close(state);
        return 2;
    }
    status = luau_load(state, chunkname, bytecode, bytecode_size, 0);
    free(bytecode);
    if (status == 0 && getenv("LUAUC_DUMP_BYTECODE") != NULL)
        __dump_proto(clvalue(state->top - 1)->l.p, 0);
    if (status == 0)
        status = lua_resume(state, NULL, 0);
    while (strcmp(basename, "debugger.luau") == 0 && status == LUA_BREAK)
    {
        if (__conformance_interrupted_thread != NULL)
        {
            (void)lua_resume(__conformance_interrupted_thread, NULL, 0);
            __conformance_interrupted_thread = NULL;
        }
        status = lua_resume(state, NULL, 0);
    }
    if (strcmp(basename, "ndebug_upvalues.luau") == 0 && status == LUA_YIELD)
        status = lua_resume(state, NULL, 0);
    if (status != 0)
    {
        fprintf(
            stderr,
            "%s\n%s\n",
            lua_tostring(state, -1) != NULL ? lua_tostring(state, -1) : "(non-string error)",
            lua_debugtrace(state)
        );
        lua_close(state);
        return 1;
    }
    if (!lua_isstring(state, -1) || strcmp(lua_tostring(state, -1), "OK") != 0)
    {
        fprintf(stderr, "fixture did not return OK\n");
        lua_close(state);
        return 1;
    }
    lua_close(state);
    return 0;
}
