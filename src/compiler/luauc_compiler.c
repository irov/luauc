// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "compiler/luauc_compiler.h"

#include "bytecode/luauc_bytecode_builder.h"
#include "compiler/luauc_compile_constant.h"

#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    LUAUC_MAX_REGISTERS = 255,
    LUAUC_MAX_UPVALUES = 200,
    LUAUC_MAX_LOCALS = 200
};

typedef struct luauc_compiler_function_t
{
    luauc_ast_expr_t* expression;
    uint32_t id;
    luauc_vector_t upvalues;
} luauc_compiler_function_t;

typedef struct luauc_compiler_local_t
{
    luauc_ast_local_t* local;
    uint8_t reg;
    int captured;
    int written;
    uint32_t startpc;
} luauc_compiler_local_t;

typedef enum luauc_loop_jump_kind_t
{
    LUAUC_LOOP_BREAK,
    LUAUC_LOOP_CONTINUE
} luauc_loop_jump_kind_t;

typedef struct luauc_loop_jump_t
{
    luauc_loop_jump_kind_t kind;
    size_t label;
} luauc_loop_jump_t;

typedef struct luauc_compiler_loop_t
{
    size_t jump_start;
    size_t local_start;
    size_t continue_local_start;
} luauc_compiler_loop_t;

typedef struct luauc_owned_string_t
{
    char* data;
    size_t size;
} luauc_owned_string_t;

typedef struct luauc_compiler_userdata_type_t
{
    luauc_string_ref_t name;
    uint32_t index;
} luauc_compiler_userdata_type_t;

typedef enum luauc_lvalue_kind_t
{
    LUAUC_LVALUE_LOCAL,
    LUAUC_LVALUE_UPVALUE,
    LUAUC_LVALUE_GLOBAL,
    LUAUC_LVALUE_INDEX_NAME,
    LUAUC_LVALUE_INDEX_NUMBER,
    LUAUC_LVALUE_INDEX_EXPR
} luauc_lvalue_kind_t;

typedef struct luauc_lvalue_t
{
    luauc_lvalue_kind_t kind;
    uint8_t reg;
    uint8_t upvalue;
    uint8_t index;
    uint8_t number;
    luauc_string_ref_t name;
    luauc_location_t location;
} luauc_lvalue_t;

typedef struct luauc_compiler_t
{
    luauc_allocator_t allocator;
    luauc_bytecode_builder_t bytecode;
    lua_CompileOptions options;
    luauc_vector_t functions;
    luauc_vector_t locals;
    luauc_vector_t loops;
    luauc_vector_t loop_jumps;
    luauc_vector_t owned_strings;
    luauc_vector_t mutable_globals;
    luauc_vector_t userdata_types;
    luauc_compiler_function_t* current_function;
    luauc_ast_stat_t* current_body;
    size_t argument_count;
    unsigned int register_top;
    unsigned int stack_size;
    int failed;
    luauc_location_t error_location;
    char* error_message;
    size_t error_capacity;
    jmp_buf error_jump;
} luauc_compiler_t;

static void __luauc_compile_statement(luauc_compiler_t* compiler, luauc_ast_stat_t* statement);
static void __luauc_compile_expression(luauc_compiler_t* compiler, luauc_ast_expr_t* expression, uint8_t target);
static void __luauc_collect_statement_functions(luauc_compiler_t* compiler, luauc_ast_stat_t* statement);
static int __luauc_statement_writes_local(const luauc_ast_stat_t* statement, const luauc_ast_local_t* local);
static luauc_bytecode_type_t __luauc_compiler_type(
    luauc_compiler_t* compiler,
    const luauc_ast_type_t* type,
    const luauc_ast_expr_t* function
);

static void __luauc_compiler_raise(luauc_compiler_t* compiler, luauc_location_t location, const char* format, ...)
{
    va_list arguments;
    va_list copy;
    int length;
    void* memory;

    compiler->error_location = location;
    va_start(arguments, format);
    va_copy(copy, arguments);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0)
    {
        va_end(arguments);
        compiler->failed = 1;
        longjmp(compiler->error_jump, 1);
    }
    if ((size_t)length + 1 > compiler->error_capacity)
    {
        memory = compiler->allocator.reallocate(
            compiler->allocator.context,
            compiler->error_message,
            compiler->error_capacity,
            (size_t)length + 1
        );
        if (memory == NULL)
        {
            va_end(arguments);
            compiler->failed = 1;
            longjmp(compiler->error_jump, 1);
        }
        compiler->error_message = (char*)memory;
        compiler->error_capacity = (size_t)length + 1;
    }
    vsnprintf(compiler->error_message, compiler->error_capacity, format, arguments);
    va_end(arguments);
    longjmp(compiler->error_jump, 1);
}

static void __luauc_compiler_check(luauc_compiler_t* compiler, int condition, luauc_location_t location)
{
    if (!condition)
        __luauc_compiler_raise(compiler, location, "Out of memory");
}

static luauc_string_ref_t __luauc_name_ref(luauc_name_t name)
{
    luauc_string_ref_t result;
    result.data = name.value != NULL ? name.value : "";
    result.length = name.value != NULL ? strlen(name.value) : 0;
    return result;
}

static luauc_string_ref_t __luauc_array_ref(luauc_array_t value)
{
    luauc_string_ref_t result;
    result.data = (const char*)value.data;
    result.length = value.size;
    return result;
}

static void __luauc_set_line(luauc_compiler_t* compiler, luauc_location_t location)
{
    if (compiler->options.debugLevel >= 1)
        luauc_bytecode_set_debug_line(&compiler->bytecode, (int)location.begin.line + 1);
}

static uint8_t __luauc_allocate_registers(luauc_compiler_t* compiler, unsigned int count, luauc_location_t location)
{
    unsigned int result = compiler->register_top;
    if (count > LUAUC_MAX_REGISTERS || result > LUAUC_MAX_REGISTERS - count)
        __luauc_compiler_raise(
            compiler,
            location,
            "Out of registers when trying to allocate %u registers: exceeded limit %d",
            count,
            LUAUC_MAX_REGISTERS
        );
    compiler->register_top += count;
    if (compiler->stack_size < compiler->register_top)
        compiler->stack_size = compiler->register_top;
    return (uint8_t)result;
}

static luauc_compiler_local_t* __luauc_find_local(luauc_compiler_t* compiler, luauc_ast_local_t* local)
{
    size_t index;
    for (index = compiler->locals.size; index > 0; --index)
    {
        luauc_compiler_local_t* entry =
            (luauc_compiler_local_t*)luauc_vector_at(&compiler->locals, index - 1);
        if (entry->local == local)
            return entry;
    }
    return NULL;
}

static void __luauc_push_local(
    luauc_compiler_t* compiler, luauc_ast_local_t* local, uint8_t reg, uint32_t startpc
)
{
    luauc_compiler_local_t entry;
    if (compiler->locals.size >= LUAUC_MAX_LOCALS)
        __luauc_compiler_raise(
            compiler,
            local->location,
            "Out of local registers when trying to allocate %s: exceeded limit %d",
            local->name.value,
            LUAUC_MAX_LOCALS
        );
    entry.local = local;
    entry.reg = reg;
    entry.captured = 0;
    entry.written = __luauc_statement_writes_local(compiler->current_body, local);
    entry.startpc = startpc;
    __luauc_compiler_check(
        compiler,
        luauc_vector_push(&compiler->locals, &entry) != NULL,
        local->location
    );
}

static void __luauc_close_locals(luauc_compiler_t* compiler, size_t start)
{
    size_t index;
    int captured = 0;
    uint8_t first = UINT8_MAX;
    uint32_t endpc = luauc_bytecode_get_debug_pc(&compiler->bytecode);

    for (index = start; index < compiler->locals.size; ++index)
    {
        luauc_compiler_local_t* entry =
            (luauc_compiler_local_t*)luauc_vector_at(&compiler->locals, index);
        if (entry->captured)
        {
            captured = 1;
            if (entry->reg < first)
                first = entry->reg;
        }
        if (compiler->options.debugLevel >= 2)
            luauc_bytecode_push_debug_local(
                &compiler->bytecode,
                __luauc_name_ref(entry->local->name),
                entry->reg,
                entry->startpc,
                endpc
            );
        if (compiler->options.typeInfoLevel >= 1 &&
            index >= compiler->argument_count)
            luauc_bytecode_push_local_type_info(
                &compiler->bytecode,
                __luauc_compiler_type(
                    compiler,
                    entry->local->annotation,
                    compiler->current_function != NULL ?
                        compiler->current_function->expression : NULL
                ),
                entry->reg,
                entry->startpc,
                endpc
            );
    }
    if (captured)
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_CLOSEUPVALS, first, 0, 0);
}

static void __luauc_pop_locals(luauc_compiler_t* compiler, size_t start)
{
    if (start <= compiler->locals.size)
        compiler->locals.size = start;
}

static uint8_t __luauc_get_upvalue(luauc_compiler_t* compiler, luauc_ast_local_t* local)
{
    size_t index;
    for (index = 0; index < compiler->current_function->upvalues.size; ++index)
    {
        if (*(luauc_ast_local_t**)luauc_vector_at(&compiler->current_function->upvalues, index) == local)
            return (uint8_t)index;
    }
    if (compiler->current_function->upvalues.size >= LUAUC_MAX_UPVALUES)
        __luauc_compiler_raise(
            compiler,
            local->location,
            "Out of upvalue registers when trying to allocate %s: exceeded limit %d",
            local->name.value,
            LUAUC_MAX_UPVALUES
        );
    __luauc_compiler_check(
        compiler,
        luauc_vector_push(&compiler->current_function->upvalues, &local) != NULL,
        local->location
    );
    return (uint8_t)(compiler->current_function->upvalues.size - 1);
}

static luauc_compiler_function_t* __luauc_find_function(
    luauc_compiler_t* compiler, const luauc_ast_expr_t* expression
)
{
    size_t index;
    for (index = 0; index < compiler->functions.size; ++index)
    {
        luauc_compiler_function_t* function =
            (luauc_compiler_function_t*)luauc_vector_at(&compiler->functions, index);
        if (function->expression == expression)
            return function;
    }
    return NULL;
}

static int32_t __luauc_add_string(
    luauc_compiler_t* compiler, luauc_string_ref_t string, luauc_location_t location
)
{
    int32_t result = luauc_bytecode_add_constant_string(&compiler->bytecode, string);
    if (result < 0)
        __luauc_compiler_raise(compiler, location, "Exceeded constant limit; simplify the code to compile");
    return result;
}

static void __luauc_emit_load_constant(luauc_compiler_t* compiler, uint8_t target, int32_t constant)
{
    if (constant < 32768)
        luauc_bytecode_emit_ad(&compiler->bytecode, LOP_LOADK, target, (int16_t)constant);
    else
    {
        luauc_bytecode_emit_ad(&compiler->bytecode, LOP_LOADKX, target, 0);
        luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
    }
}

static void __luauc_emit_number_constant(
    luauc_compiler_t* compiler, uint8_t target, double value, luauc_location_t location
)
{
    int32_t constant;
    if (compiler->options.optimizationLevel >= 1 &&
        value >= INT16_MIN && value <= INT16_MAX &&
        value == (double)(int16_t)value && !(value == 0.0 && signbit(value)))
    {
        luauc_bytecode_emit_ad(
            &compiler->bytecode, LOP_LOADN, target, (int16_t)value
        );
        return;
    }
    constant = luauc_bytecode_add_constant_number(&compiler->bytecode, value);
    if (constant < 0)
        __luauc_compiler_raise(
            compiler, location, "Exceeded constant limit; simplify the code to compile"
        );
    __luauc_emit_load_constant(compiler, target, constant);
}

static int __luauc_string_ref_equals_cstring(luauc_string_ref_t value, const char* text)
{
    size_t length;
    if (value.data == NULL || text == NULL)
        return 0;
    length = strlen(text);
    return value.length == length && memcmp(value.data, text, length) == 0;
}

static int __luauc_string_list_contains(const char* const* list, luauc_string_ref_t value)
{
    const char* const* item;
    if (list == NULL)
        return 0;
    for (item = list; *item != NULL; ++item)
        if (__luauc_string_ref_equals_cstring(value, *item))
            return 1;
    return 0;
}

static int __luauc_compiler_global_is_mutable(
    const luauc_compiler_t* compiler, luauc_string_ref_t name
)
{
    size_t index;
    if (__luauc_string_list_contains(compiler->options.mutableGlobals, name))
        return 1;
    for (index = 0; index < compiler->mutable_globals.size; ++index)
    {
        const luauc_string_ref_t* current =
            (const luauc_string_ref_t*)luauc_vector_at_const(
                &compiler->mutable_globals, index
            );
        if (current->length == name.length &&
            memcmp(current->data, name.data, name.length) == 0)
            return 1;
    }
    return 0;
}

static int __luauc_function_has_generic(
    const luauc_ast_expr_t* function, luauc_string_ref_t name
)
{
    size_t index;
    if (function == NULL || function->kind != LUAUC_EXPR_FUNCTION)
        return 0;
    for (index = 0; index < function->value.function.generics.size; ++index)
    {
        const luauc_ast_generic_type_t* generic =
            ((luauc_ast_generic_type_t**)function->value.function.generics.data)[index];
        if (generic->name.value != NULL &&
            __luauc_string_ref_equals_cstring(name, generic->name.value))
            return 1;
    }
    return 0;
}

static int __luauc_find_userdata_type(
    luauc_compiler_t* compiler, luauc_string_ref_t name
)
{
    size_t index;

    for (index = compiler->userdata_types.size; index > 0; --index)
    {
        const luauc_compiler_userdata_type_t* type =
            (const luauc_compiler_userdata_type_t*)luauc_vector_at_const(
                &compiler->userdata_types, index - 1
            );
        if (type->name.length == name.length &&
            memcmp(type->name.data, name.data, name.length) == 0)
        {
            luauc_bytecode_use_userdata_type(&compiler->bytecode, type->index);
            return (int)type->index;
        }
    }
    return -1;
}

static void __luauc_initialize_userdata_types(
    luauc_compiler_t* compiler,
    const luauc_name_table_t* names,
    luauc_location_t location
)
{
    const char* const* option;
    size_t count = 0;

    if (compiler->options.userdataTypes == NULL)
        return;
    for (option = compiler->options.userdataTypes; *option != NULL; ++option)
        ++count;
    if (count > (size_t)(LBC_TYPE_TAGGED_USERDATA_END - LBC_TYPE_TAGGED_USERDATA_BASE))
        __luauc_compiler_raise(
            compiler, location, "Exceeded userdata type limit in the compilation options"
        );

    for (option = compiler->options.userdataTypes; *option != NULL; ++option)
    {
        luauc_token_type_t ignored;
        luauc_name_t interned = luauc_name_table_get(
            names, *option, strlen(*option), &ignored
        );
        if (interned.value != NULL)
        {
            luauc_compiler_userdata_type_t type;
            type.name = __luauc_name_ref(interned);
            type.index = luauc_bytecode_add_userdata_type(
                &compiler->bytecode, interned.value
            );
            if (type.index == UINT32_MAX ||
                luauc_vector_push(&compiler->userdata_types, &type) == NULL)
                __luauc_compiler_raise(compiler, location, "Out of memory");
        }
    }
}

static luauc_bytecode_type_t __luauc_compiler_type(
    luauc_compiler_t* compiler,
    const luauc_ast_type_t* type,
    const luauc_ast_expr_t* function
)
{
    if (type == NULL)
        return LBC_TYPE_ANY;
    switch (type->kind)
    {
    case LUAUC_TYPE_REFERENCE:
        if (type->value.reference.has_prefix)
            return LBC_TYPE_ANY;
        {
            luauc_string_ref_t name = __luauc_name_ref(type->value.reference.name);
            int userdata;
            if (__luauc_function_has_generic(function, name))
                return LBC_TYPE_ANY;
            if (__luauc_string_ref_equals_cstring(name, "nil"))
                return LBC_TYPE_NIL;
            if (__luauc_string_ref_equals_cstring(name, "boolean"))
                return LBC_TYPE_BOOLEAN;
            if (__luauc_string_ref_equals_cstring(name, "number"))
                return LBC_TYPE_NUMBER;
            if (__luauc_string_ref_equals_cstring(name, "integer"))
                return LBC_TYPE_INTEGER;
            if (__luauc_string_ref_equals_cstring(name, "string"))
                return LBC_TYPE_STRING;
            if (__luauc_string_ref_equals_cstring(name, "thread"))
                return LBC_TYPE_THREAD;
            if (__luauc_string_ref_equals_cstring(name, "buffer"))
                return LBC_TYPE_BUFFER;
            if (__luauc_string_ref_equals_cstring(name, "vector") ||
                __luauc_string_ref_equals_cstring(
                    name, compiler->options.vectorType
                ))
                return LBC_TYPE_VECTOR;
            if (__luauc_string_ref_equals_cstring(name, "any") ||
                __luauc_string_ref_equals_cstring(name, "unknown"))
                return LBC_TYPE_ANY;
            userdata = __luauc_find_userdata_type(compiler, name);
            return userdata >= 0 ?
                (luauc_bytecode_type_t)(LBC_TYPE_TAGGED_USERDATA_BASE + userdata) :
                LBC_TYPE_USERDATA;
        }
    case LUAUC_TYPE_TABLE:
        return LBC_TYPE_TABLE;
    case LUAUC_TYPE_FUNCTION:
        return LBC_TYPE_FUNCTION;
    case LUAUC_TYPE_SINGLETON_BOOL:
        return LBC_TYPE_BOOLEAN;
    case LUAUC_TYPE_SINGLETON_STRING:
        return LBC_TYPE_STRING;
    case LUAUC_TYPE_GROUP:
        return __luauc_compiler_type(compiler, type->value.group.type, function);
    case LUAUC_TYPE_UNION:
    {
        luauc_ast_type_t** parts = (luauc_ast_type_t**)type->value.aggregate.types.data;
        luauc_bytecode_type_t result = LBC_TYPE_INVALID;
        int optional = 0;
        size_t index;
        for (index = 0; index < type->value.aggregate.types.size; ++index)
        {
            luauc_bytecode_type_t part =
                parts[index]->kind == LUAUC_TYPE_OPTIONAL ? LBC_TYPE_NIL :
                    __luauc_compiler_type(compiler, parts[index], function);
            if (part == LBC_TYPE_NIL)
            {
                optional = 1;
                continue;
            }
            if (result == LBC_TYPE_INVALID)
                result = part;
            else if (result != part)
                return LBC_TYPE_ANY;
        }
        if (result == LBC_TYPE_INVALID)
            return LBC_TYPE_ANY;
        return result == LBC_TYPE_ANY || !optional ? result :
            (luauc_bytecode_type_t)(result | LBC_TYPE_OPTIONAL_BIT);
    }
    default:
        return LBC_TYPE_ANY;
    }
}

static void __luauc_set_function_type_info(
    luauc_compiler_t* compiler, const luauc_ast_expr_t* function
)
{
    unsigned char data[258];
    luauc_ast_local_t** arguments;
    size_t argument_count;
    size_t offset = 0;
    size_t index;
    int has_concrete_argument = 0;

    if (function == NULL || (compiler->options.typeInfoLevel < 1 &&
            compiler->options.optimizationLevel < 2))
        return;
    arguments = (luauc_ast_local_t**)function->value.function.arguments.data;
    argument_count = function->value.function.arguments.size;
    data[offset++] = LBC_TYPE_FUNCTION;
    data[offset++] = (unsigned char)(
        argument_count + (function->value.function.self != NULL ? 1 : 0)
    );
    if (function->value.function.self != NULL)
        data[offset++] = LBC_TYPE_TABLE;
    for (index = 0; index < argument_count; ++index)
    {
        luauc_bytecode_type_t type = __luauc_compiler_type(
            compiler, arguments[index]->annotation, function
        );
        if (type != LBC_TYPE_ANY)
            has_concrete_argument = 1;
        data[offset++] = (unsigned char)type;
    }
    if (has_concrete_argument &&
        !luauc_bytecode_set_function_type_info(
            &compiler->bytecode, data, offset
        ))
        __luauc_compiler_raise(compiler, function->location, "Out of memory");
}

static int __luauc_emit_compile_constant(
    luauc_compiler_t* compiler,
    uint8_t target,
    const luauc_compile_constant_value_t* value,
    luauc_location_t location
)
{
    int32_t constant;
    switch (value->type)
    {
    case LUAUC_COMPILE_CONSTANT_NIL:
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_LOADNIL, target, 0, 0);
        return 1;
    case LUAUC_COMPILE_CONSTANT_BOOLEAN:
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_LOADB,
            target,
            (uint8_t)(value->value.boolean_value != 0),
            0
        );
        return 1;
    case LUAUC_COMPILE_CONSTANT_NUMBER:
        __luauc_emit_number_constant(
            compiler, target, value->value.number_value, location
        );
        return 1;
    case LUAUC_COMPILE_CONSTANT_INTEGER:
        constant = luauc_bytecode_add_constant_integer(
            &compiler->bytecode, value->value.integer_value
        );
        break;
    case LUAUC_COMPILE_CONSTANT_VECTOR:
        constant = luauc_bytecode_add_constant_vector(
            &compiler->bytecode,
            value->value.vector_value[0],
            value->value.vector_value[1],
            value->value.vector_value[2],
            value->value.vector_value[3]
        );
        break;
    case LUAUC_COMPILE_CONSTANT_STRING:
        if (value->value.string_value.data == NULL && value->value.string_value.size != 0)
            return 0;
        constant = luauc_bytecode_add_constant_string(
            &compiler->bytecode,
            (luauc_string_ref_t){
                value->value.string_value.data != NULL ? value->value.string_value.data : "",
                value->value.string_value.size
            }
        );
        break;
    case LUAUC_COMPILE_CONSTANT_UNKNOWN:
    default:
        return 0;
    }

    if (constant < 0)
        __luauc_compiler_raise(
            compiler, location, "Exceeded constant limit; simplify the code to compile"
        );
    __luauc_emit_load_constant(compiler, target, constant);
    return 1;
}

static int __luauc_try_compile_known_library_member(
    luauc_compiler_t* compiler,
    const luauc_ast_expr_t* expression,
    luauc_string_ref_t member,
    uint8_t target
)
{
    const luauc_ast_expr_t* object = expression->value.index_name.expression;
    luauc_string_ref_t library;
    luauc_compile_constant_value_t constant;

    if (object->kind != LUAUC_EXPR_GLOBAL)
        return 0;
    library = __luauc_name_ref(object->value.global.name);

    if ((compiler->options.typeInfoLevel >= 1 || compiler->options.optimizationLevel >= 2) &&
        compiler->options.libraryMemberTypeCb != NULL)
        (void)compiler->options.libraryMemberTypeCb(library.data, member.data);

    if (compiler->options.optimizationLevel < 2 ||
        compiler->options.libraryMemberConstantCb == NULL ||
        __luauc_compiler_global_is_mutable(compiler, library) ||
        (!__luauc_string_ref_equals_cstring(library, "math") &&
            !__luauc_string_list_contains(compiler->options.librariesWithKnownMembers, library)))
        return 0;

    memset(&constant, 0, sizeof(constant));
    compiler->options.libraryMemberConstantCb(
        library.data, member.data, (lua_CompileConstant*)&constant
    );
    return __luauc_emit_compile_constant(
        compiler, target, &constant, expression->location
    );
}

static int __luauc_try_compile_import(
    luauc_compiler_t* compiler, const luauc_ast_expr_t* expression, uint8_t target
)
{
    const luauc_ast_expr_t* cursor = expression;
    luauc_string_ref_t reverse_names[3];
    int32_t ids[3];
    size_t count = 0;
    size_t index;
    uint32_t import_id;
    int32_t constant;

    if (compiler->options.optimizationLevel < 1)
        return 0;
    while (cursor->kind == LUAUC_EXPR_INDEX_NAME && count < 3)
    {
        reverse_names[count++] = __luauc_name_ref(cursor->value.index_name.index);
        cursor = cursor->value.index_name.expression;
    }
    if (cursor->kind != LUAUC_EXPR_GLOBAL || count == 3)
        return 0;
    reverse_names[count++] = __luauc_name_ref(cursor->value.global.name);
    if (__luauc_string_ref_equals_cstring(reverse_names[count - 1], "_G") ||
        __luauc_compiler_global_is_mutable(compiler, reverse_names[count - 1]))
        return 0;

    for (index = 0; index < count; ++index)
    {
        ids[index] = __luauc_add_string(
            compiler, reverse_names[count - index - 1], expression->location
        );
        if (ids[index] >= 1024)
            return 0;
    }
    import_id = count == 1 ? luauc_bytecode_get_import_id1(ids[0]) :
        count == 2 ? luauc_bytecode_get_import_id2(ids[0], ids[1]) :
        luauc_bytecode_get_import_id3(ids[0], ids[1], ids[2]);
    constant = luauc_bytecode_add_import(&compiler->bytecode, import_id);
    if (constant < 0)
        __luauc_compiler_raise(
            compiler,
            expression->location,
            "Exceeded constant limit; simplify the code to compile"
        );
    if (constant >= 32768)
        return 0;
    luauc_bytecode_emit_ad(
        &compiler->bytecode, LOP_GETIMPORT, target, (int16_t)constant
    );
    luauc_bytecode_emit_aux(&compiler->bytecode, import_id);
    return 1;
}

static void __luauc_patch_jump(
    luauc_compiler_t* compiler, size_t source, size_t target, luauc_location_t location
)
{
    if (!luauc_bytecode_patch_jump_d(&compiler->bytecode, source, target))
        __luauc_compiler_raise(compiler, location, "Exceeded jump distance limit; simplify the code to compile");
}

static size_t __luauc_emit_condition_jump(
    luauc_compiler_t* compiler, luauc_ast_expr_t* condition, int jump_if_truthy
)
{
    unsigned int old_top = compiler->register_top;
    uint8_t reg = __luauc_allocate_registers(compiler, 1, condition->location);
    size_t label;
    __luauc_compile_expression(compiler, condition, reg);
    label = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_bytecode_emit_ad(
        &compiler->bytecode, jump_if_truthy ? LOP_JUMPIF : LOP_JUMPIFNOT, reg, 0
    );
    compiler->register_top = old_top;
    return label;
}

static int __luauc_expression_is_multret(const luauc_ast_expr_t* expression)
{
    return expression->kind == LUAUC_EXPR_CALL || expression->kind == LUAUC_EXPR_VARARGS;
}

static int __luauc_statement_terminates(const luauc_ast_stat_t* statement)
{
    size_t index;
    if (statement == NULL)
        return 0;
    switch (statement->kind)
    {
    case LUAUC_STAT_RETURN:
    case LUAUC_STAT_BREAK:
    case LUAUC_STAT_CONTINUE:
        return 1;
    case LUAUC_STAT_BLOCK:
        if (statement->value.block.body.size == 0)
            return 0;
        index = statement->value.block.body.size - 1;
        return __luauc_statement_terminates(
            ((luauc_ast_stat_t**)statement->value.block.body.data)[index]
        );
    case LUAUC_STAT_IF:
        return statement->value.if_statement.else_body != NULL &&
            __luauc_statement_terminates(statement->value.if_statement.then_body) &&
            __luauc_statement_terminates(statement->value.if_statement.else_body);
    default:
        return 0;
    }
}

static int __luauc_expression_writes_local(
    const luauc_ast_expr_t* expression, const luauc_ast_local_t* local
)
{
    size_t index;
    if (expression == NULL)
        return 0;
    switch (expression->kind)
    {
    case LUAUC_EXPR_GROUP:
        return __luauc_expression_writes_local(expression->value.group.expression, local);
    case LUAUC_EXPR_CALL:
        if (__luauc_expression_writes_local(expression->value.call.function, local))
            return 1;
        for (index = 0; index < expression->value.call.arguments.size; ++index)
            if (__luauc_expression_writes_local(
                    ((luauc_ast_expr_t**)expression->value.call.arguments.data)[index], local
                ))
                return 1;
        return 0;
    case LUAUC_EXPR_INDEX_NAME:
        return __luauc_expression_writes_local(expression->value.index_name.expression, local);
    case LUAUC_EXPR_INDEX_EXPR:
        return __luauc_expression_writes_local(expression->value.index_expr.expression, local) ||
            __luauc_expression_writes_local(expression->value.index_expr.index, local);
    case LUAUC_EXPR_FUNCTION:
        return __luauc_statement_writes_local(expression->value.function.body, local);
    case LUAUC_EXPR_TABLE:
        for (index = 0; index < expression->value.table.items.size; ++index)
        {
            const luauc_ast_table_item_t* item =
                &((const luauc_ast_table_item_t*)expression->value.table.items.data)[index];
            if (__luauc_expression_writes_local(item->key, local) ||
                __luauc_expression_writes_local(item->value, local))
                return 1;
        }
        return 0;
    case LUAUC_EXPR_UNARY:
        return __luauc_expression_writes_local(expression->value.unary.expression, local);
    case LUAUC_EXPR_BINARY:
        return __luauc_expression_writes_local(expression->value.binary.left, local) ||
            __luauc_expression_writes_local(expression->value.binary.right, local);
    case LUAUC_EXPR_TYPE_ASSERTION:
        return __luauc_expression_writes_local(
            expression->value.type_assertion.expression, local
        );
    case LUAUC_EXPR_IF_ELSE:
        return __luauc_expression_writes_local(expression->value.if_else.condition, local) ||
            __luauc_expression_writes_local(expression->value.if_else.true_expression, local) ||
            __luauc_expression_writes_local(expression->value.if_else.false_expression, local);
    case LUAUC_EXPR_INTERP_STRING:
        for (index = 0; index < expression->value.interpolated_string.expressions.size; ++index)
            if (__luauc_expression_writes_local(
                    ((luauc_ast_expr_t**)expression->value.interpolated_string.expressions.data)[index],
                    local
                ))
                return 1;
        return 0;
    case LUAUC_EXPR_INSTANTIATE:
        return __luauc_expression_writes_local(expression->value.instantiate.expression, local);
    default:
        return 0;
    }
}

static int __luauc_assignment_writes_local(
    luauc_array_t variables, const luauc_ast_local_t* local
)
{
    size_t index;
    for (index = 0; index < variables.size; ++index)
    {
        const luauc_ast_expr_t* variable = ((luauc_ast_expr_t**)variables.data)[index];
        if (variable->kind == LUAUC_EXPR_LOCAL && variable->value.local.local == local)
            return 1;
    }
    return 0;
}

static int __luauc_statement_writes_local(
    const luauc_ast_stat_t* statement, const luauc_ast_local_t* local
)
{
    size_t index;
    if (statement == NULL)
        return 0;
    switch (statement->kind)
    {
    case LUAUC_STAT_BLOCK:
        for (index = 0; index < statement->value.block.body.size; ++index)
            if (__luauc_statement_writes_local(
                    ((luauc_ast_stat_t**)statement->value.block.body.data)[index], local
                ))
                return 1;
        return 0;
    case LUAUC_STAT_IF:
        return __luauc_expression_writes_local(statement->value.if_statement.condition, local) ||
            __luauc_statement_writes_local(statement->value.if_statement.then_body, local) ||
            __luauc_statement_writes_local(statement->value.if_statement.else_body, local);
    case LUAUC_STAT_WHILE:
        return __luauc_expression_writes_local(statement->value.while_statement.condition, local) ||
            __luauc_statement_writes_local(statement->value.while_statement.body, local);
    case LUAUC_STAT_REPEAT:
        return __luauc_statement_writes_local(statement->value.repeat_statement.body, local) ||
            __luauc_expression_writes_local(statement->value.repeat_statement.condition, local);
    case LUAUC_STAT_RETURN:
        for (index = 0; index < statement->value.return_statement.expressions.size; ++index)
            if (__luauc_expression_writes_local(
                    ((luauc_ast_expr_t**)statement->value.return_statement.expressions.data)[index],
                    local
                ))
                return 1;
        return 0;
    case LUAUC_STAT_EXPR:
        return __luauc_expression_writes_local(statement->value.expression.expression, local);
    case LUAUC_STAT_LOCAL:
        for (index = 0; index < statement->value.local.values.size; ++index)
            if (__luauc_expression_writes_local(
                    ((luauc_ast_expr_t**)statement->value.local.values.data)[index], local
                ))
                return 1;
        return 0;
    case LUAUC_STAT_FOR:
        return __luauc_expression_writes_local(statement->value.for_statement.from, local) ||
            __luauc_expression_writes_local(statement->value.for_statement.to, local) ||
            __luauc_expression_writes_local(statement->value.for_statement.step, local) ||
            __luauc_statement_writes_local(statement->value.for_statement.body, local);
    case LUAUC_STAT_FOR_IN:
        for (index = 0; index < statement->value.for_in.values.size; ++index)
            if (__luauc_expression_writes_local(
                    ((luauc_ast_expr_t**)statement->value.for_in.values.data)[index], local
                ))
                return 1;
        return __luauc_statement_writes_local(statement->value.for_in.body, local);
    case LUAUC_STAT_ASSIGN:
        if (__luauc_assignment_writes_local(statement->value.assign.variables, local))
            return 1;
        for (index = 0; index < statement->value.assign.variables.size; ++index)
            if (__luauc_expression_writes_local(
                    ((luauc_ast_expr_t**)statement->value.assign.variables.data)[index], local
                ))
                return 1;
        for (index = 0; index < statement->value.assign.values.size; ++index)
            if (__luauc_expression_writes_local(
                    ((luauc_ast_expr_t**)statement->value.assign.values.data)[index], local
                ))
                return 1;
        return 0;
    case LUAUC_STAT_COMPOUND_ASSIGN:
        if (statement->value.compound_assign.variable->kind == LUAUC_EXPR_LOCAL &&
            statement->value.compound_assign.variable->value.local.local == local)
            return 1;
        return __luauc_expression_writes_local(
                   statement->value.compound_assign.variable, local
               ) ||
            __luauc_expression_writes_local(statement->value.compound_assign.value, local);
    case LUAUC_STAT_FUNCTION:
        if (statement->value.function.name->kind == LUAUC_EXPR_LOCAL &&
            statement->value.function.name->value.local.local == local)
            return 1;
        return __luauc_expression_writes_local(statement->value.function.function, local);
    case LUAUC_STAT_LOCAL_FUNCTION:
        if (statement->value.local_function.name == local)
            return 1;
        return __luauc_expression_writes_local(
            statement->value.local_function.function, local
        );
    case LUAUC_STAT_CLASS:
        for (index = 0; index < statement->value.class_statement.members.size; ++index)
        {
            const luauc_class_member_t* member =
                &((const luauc_class_member_t*)statement->value.class_statement.members.data)[index];
            if (member->is_method &&
                __luauc_expression_writes_local(member->value.method.function, local))
                return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static void __luauc_collect_expression_functions(luauc_compiler_t* compiler, luauc_ast_expr_t* expression)
{
    size_t index;
    if (expression == NULL)
        return;

    switch (expression->kind)
    {
    case LUAUC_EXPR_GROUP:
        __luauc_collect_expression_functions(compiler, expression->value.group.expression);
        break;
    case LUAUC_EXPR_CALL:
        __luauc_collect_expression_functions(compiler, expression->value.call.function);
        for (index = 0; index < expression->value.call.arguments.size; ++index)
            __luauc_collect_expression_functions(
                compiler, ((luauc_ast_expr_t**)expression->value.call.arguments.data)[index]
            );
        break;
    case LUAUC_EXPR_INDEX_NAME:
        __luauc_collect_expression_functions(compiler, expression->value.index_name.expression);
        break;
    case LUAUC_EXPR_INDEX_EXPR:
        __luauc_collect_expression_functions(compiler, expression->value.index_expr.expression);
        __luauc_collect_expression_functions(compiler, expression->value.index_expr.index);
        break;
    case LUAUC_EXPR_FUNCTION:
    {
        luauc_compiler_function_t function;
        if (__luauc_find_function(compiler, expression) != NULL)
            break;
        __luauc_collect_statement_functions(compiler, expression->value.function.body);
        memset(&function, 0, sizeof(function));
        function.expression = expression;
        function.id = UINT32_MAX;
        __luauc_compiler_check(
            compiler,
            luauc_vector_init(
                &function.upvalues, sizeof(luauc_ast_local_t*), compiler->allocator
            ),
            expression->location
        );
        if (luauc_vector_push(&compiler->functions, &function) == NULL)
        {
            luauc_vector_destroy(&function.upvalues);
            __luauc_compiler_raise(compiler, expression->location, "Out of memory");
        }
        break;
    }
    case LUAUC_EXPR_TABLE:
        for (index = 0; index < expression->value.table.items.size; ++index)
        {
            luauc_ast_table_item_t* item =
                &((luauc_ast_table_item_t*)expression->value.table.items.data)[index];
            __luauc_collect_expression_functions(compiler, item->key);
            __luauc_collect_expression_functions(compiler, item->value);
        }
        break;
    case LUAUC_EXPR_UNARY:
        __luauc_collect_expression_functions(compiler, expression->value.unary.expression);
        break;
    case LUAUC_EXPR_BINARY:
        __luauc_collect_expression_functions(compiler, expression->value.binary.left);
        __luauc_collect_expression_functions(compiler, expression->value.binary.right);
        break;
    case LUAUC_EXPR_TYPE_ASSERTION:
        __luauc_collect_expression_functions(compiler, expression->value.type_assertion.expression);
        break;
    case LUAUC_EXPR_IF_ELSE:
        __luauc_collect_expression_functions(compiler, expression->value.if_else.condition);
        __luauc_collect_expression_functions(compiler, expression->value.if_else.true_expression);
        __luauc_collect_expression_functions(compiler, expression->value.if_else.false_expression);
        break;
    case LUAUC_EXPR_INTERP_STRING:
        for (index = 0; index < expression->value.interpolated_string.expressions.size; ++index)
            __luauc_collect_expression_functions(
                compiler,
                ((luauc_ast_expr_t**)expression->value.interpolated_string.expressions.data)[index]
            );
        break;
    case LUAUC_EXPR_INSTANTIATE:
        __luauc_collect_expression_functions(compiler, expression->value.instantiate.expression);
        break;
    case LUAUC_EXPR_ERROR:
        for (index = 0; index < expression->value.error.expressions.size; ++index)
            __luauc_collect_expression_functions(
                compiler, ((luauc_ast_expr_t**)expression->value.error.expressions.data)[index]
            );
        break;
    default:
        break;
    }
}

static void __luauc_mark_mutable_global(
    luauc_compiler_t* compiler, const luauc_ast_expr_t* expression
)
{
    luauc_string_ref_t name;
    if (expression == NULL || expression->kind != LUAUC_EXPR_GLOBAL)
        return;
    name = __luauc_name_ref(expression->value.global.name);
    if (!__luauc_compiler_global_is_mutable(compiler, name) &&
        luauc_vector_push(&compiler->mutable_globals, &name) == NULL)
        __luauc_compiler_raise(compiler, expression->location, "Out of memory");
}

static void __luauc_collect_statement_functions(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    size_t index;
    if (statement == NULL)
        return;

    switch (statement->kind)
    {
    case LUAUC_STAT_BLOCK:
        for (index = 0; index < statement->value.block.body.size; ++index)
            __luauc_collect_statement_functions(
                compiler, ((luauc_ast_stat_t**)statement->value.block.body.data)[index]
            );
        break;
    case LUAUC_STAT_IF:
        __luauc_collect_expression_functions(compiler, statement->value.if_statement.condition);
        __luauc_collect_statement_functions(compiler, statement->value.if_statement.then_body);
        __luauc_collect_statement_functions(compiler, statement->value.if_statement.else_body);
        break;
    case LUAUC_STAT_WHILE:
        __luauc_collect_expression_functions(compiler, statement->value.while_statement.condition);
        __luauc_collect_statement_functions(compiler, statement->value.while_statement.body);
        break;
    case LUAUC_STAT_REPEAT:
        __luauc_collect_statement_functions(compiler, statement->value.repeat_statement.body);
        __luauc_collect_expression_functions(compiler, statement->value.repeat_statement.condition);
        break;
    case LUAUC_STAT_RETURN:
        for (index = 0; index < statement->value.return_statement.expressions.size; ++index)
            __luauc_collect_expression_functions(
                compiler,
                ((luauc_ast_expr_t**)statement->value.return_statement.expressions.data)[index]
            );
        break;
    case LUAUC_STAT_EXPR:
        __luauc_collect_expression_functions(compiler, statement->value.expression.expression);
        break;
    case LUAUC_STAT_LOCAL:
        for (index = 0; index < statement->value.local.values.size; ++index)
            __luauc_collect_expression_functions(
                compiler, ((luauc_ast_expr_t**)statement->value.local.values.data)[index]
            );
        break;
    case LUAUC_STAT_FOR:
        __luauc_collect_expression_functions(compiler, statement->value.for_statement.from);
        __luauc_collect_expression_functions(compiler, statement->value.for_statement.to);
        __luauc_collect_expression_functions(compiler, statement->value.for_statement.step);
        __luauc_collect_statement_functions(compiler, statement->value.for_statement.body);
        break;
    case LUAUC_STAT_FOR_IN:
        for (index = 0; index < statement->value.for_in.values.size; ++index)
            __luauc_collect_expression_functions(
                compiler, ((luauc_ast_expr_t**)statement->value.for_in.values.data)[index]
            );
        __luauc_collect_statement_functions(compiler, statement->value.for_in.body);
        break;
    case LUAUC_STAT_ASSIGN:
        for (index = 0; index < statement->value.assign.variables.size; ++index)
        {
            luauc_ast_expr_t* variable =
                ((luauc_ast_expr_t**)statement->value.assign.variables.data)[index];
            __luauc_mark_mutable_global(compiler, variable);
            __luauc_collect_expression_functions(
                compiler, variable
            );
        }
        for (index = 0; index < statement->value.assign.values.size; ++index)
            __luauc_collect_expression_functions(
                compiler, ((luauc_ast_expr_t**)statement->value.assign.values.data)[index]
            );
        break;
    case LUAUC_STAT_COMPOUND_ASSIGN:
        __luauc_mark_mutable_global(
            compiler, statement->value.compound_assign.variable
        );
        __luauc_collect_expression_functions(compiler, statement->value.compound_assign.variable);
        __luauc_collect_expression_functions(compiler, statement->value.compound_assign.value);
        break;
    case LUAUC_STAT_FUNCTION:
        __luauc_mark_mutable_global(compiler, statement->value.function.name);
        __luauc_collect_expression_functions(compiler, statement->value.function.name);
        __luauc_collect_expression_functions(compiler, statement->value.function.function);
        break;
    case LUAUC_STAT_LOCAL_FUNCTION:
        __luauc_collect_expression_functions(compiler, statement->value.local_function.function);
        break;
    case LUAUC_STAT_CLASS:
        for (index = 0; index < statement->value.class_statement.members.size; ++index)
        {
            luauc_class_member_t* member =
                &((luauc_class_member_t*)statement->value.class_statement.members.data)[index];
            if (member->is_method)
                __luauc_collect_expression_functions(compiler, member->value.method.function);
        }
        break;
    case LUAUC_STAT_ERROR:
        for (index = 0; index < statement->value.error.expressions.size; ++index)
            __luauc_collect_expression_functions(
                compiler, ((luauc_ast_expr_t**)statement->value.error.expressions.data)[index]
            );
        for (index = 0; index < statement->value.error.statements.size; ++index)
            __luauc_collect_statement_functions(
                compiler, ((luauc_ast_stat_t**)statement->value.error.statements.data)[index]
            );
        break;
    default:
        break;
    }
}

static void __luauc_compile_expression_n(
    luauc_compiler_t* compiler, luauc_ast_expr_t* expression, uint8_t target, uint8_t count
);

static void __luauc_compile_expression_list(
    luauc_compiler_t* compiler, luauc_array_t expressions, uint8_t target, uint8_t count
)
{
    size_t index;
    luauc_ast_expr_t** values = (luauc_ast_expr_t**)expressions.data;

    if (expressions.size == 0)
    {
        for (index = 0; index < count; ++index)
            luauc_bytecode_emit_abc(
                &compiler->bytecode, LOP_LOADNIL, (uint8_t)(target + index), 0, 0
            );
        return;
    }

    for (index = 0; index < expressions.size && index < count; ++index)
    {
        if (index + 1 == expressions.size && expressions.size < count)
            __luauc_compile_expression_n(
                compiler, values[index], (uint8_t)(target + index), (uint8_t)(count - index)
            );
        else
            __luauc_compile_expression(compiler, values[index], (uint8_t)(target + index));
    }

    if (expressions.size > count)
    {
        for (index = count; index < expressions.size; ++index)
        {
            unsigned int old_top = compiler->register_top;
            uint8_t discard = __luauc_allocate_registers(compiler, 1, values[index]->location);
            __luauc_compile_expression(compiler, values[index], discard);
            compiler->register_top = old_top;
        }
    }
}

static void __luauc_compile_call(
    luauc_compiler_t* compiler,
    luauc_ast_expr_t* expression,
    uint8_t target,
    uint8_t result_count,
    int multret
)
{
    luauc_ast_expr_t** arguments = (luauc_ast_expr_t**)expression->value.call.arguments.data;
    size_t argument_count = expression->value.call.arguments.size;
    unsigned int old_top = compiler->register_top;
    unsigned int call_register_count =
        1u + (unsigned int)argument_count + (expression->value.call.self ? 1u : 0u);
    uint8_t base;
    size_t index;
    int argument_multret = 0;
    luauc_ast_expr_t* method_function = NULL;

    if (call_register_count < result_count)
        call_register_count = result_count;
    base = __luauc_allocate_registers(compiler, call_register_count, expression->location);

    if (expression->value.call.self)
    {
        luauc_ast_expr_t* function = expression->value.call.function;
        luauc_ast_expr_t* object;

        if (function->kind != LUAUC_EXPR_INDEX_NAME)
            __luauc_compiler_raise(compiler, expression->location, "Invalid method call");
        object = function->value.index_name.expression;
        __luauc_compile_expression(compiler, object, base);
        method_function = function;
    }
    else
        __luauc_compile_expression(compiler, expression->value.call.function, base);

    for (index = 0; index < argument_count; ++index)
    {
        uint8_t argument_reg =
            (uint8_t)(base + 1 + (expression->value.call.self ? 1 : 0) + index);
        if (index + 1 == argument_count && __luauc_expression_is_multret(arguments[index]))
        {
            __luauc_compile_expression_n(compiler, arguments[index], argument_reg, 0);
            argument_multret = 1;
        }
        else
            __luauc_compile_expression(compiler, arguments[index], argument_reg);
    }

    if (method_function != NULL)
    {
        luauc_string_ref_t name = __luauc_name_ref(method_function->value.index_name.index);
        int32_t name_constant = __luauc_add_string(
            compiler, name, method_function->location
        );
        __luauc_set_line(compiler, method_function->value.index_name.index_location);
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_NAMECALL,
            base,
            base,
            (uint8_t)luauc_bytecode_get_string_hash(name)
        );
        luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)name_constant);
    }
    __luauc_set_line(compiler, expression->location);
    luauc_bytecode_emit_abc(
        &compiler->bytecode,
        LOP_CALL,
        base,
        argument_multret ? 0 : (uint8_t)(argument_count + (expression->value.call.self ? 2 : 1)),
        multret ? 0 : (uint8_t)(result_count + 1)
    );

    if (!multret)
    {
        for (index = 0; index < result_count; ++index)
            if ((uint8_t)(base + index) != (uint8_t)(target + index))
                luauc_bytecode_emit_abc(
                    &compiler->bytecode,
                    LOP_MOVE,
                    (uint8_t)(target + index),
                    (uint8_t)(base + index),
                    0
                );
    }
    else if (base != target)
    {
        /*
         * MULTRET values cannot be moved as an open tuple. Callers reserve target at
         * the current top, so this only occurs for malformed internal use.
         */
        __luauc_compiler_raise(compiler, expression->location, "Invalid multret register layout");
    }
    compiler->register_top = old_top;
}

static void __luauc_compile_expression_n(
    luauc_compiler_t* compiler, luauc_ast_expr_t* expression, uint8_t target, uint8_t count
)
{
    if (expression->kind == LUAUC_EXPR_CALL)
    {
        if (count == 0)
        {
            unsigned int old_top = compiler->register_top;
            compiler->register_top = target;
            __luauc_compile_call(compiler, expression, target, count, 1);
            compiler->register_top = old_top;
        }
        else
            __luauc_compile_call(compiler, expression, target, count, 0);
    }
    else if (expression->kind == LUAUC_EXPR_VARARGS)
        luauc_bytecode_emit_abc(
            &compiler->bytecode, LOP_GETVARARGS, target, count == 0 ? 0 : (uint8_t)(count + 1), 0
        );
    else
    {
        size_t index;
        __luauc_compile_expression(compiler, expression, target);
        for (index = 1; index < count; ++index)
            luauc_bytecode_emit_abc(
                &compiler->bytecode, LOP_LOADNIL, (uint8_t)(target + index), 0, 0
            );
    }
}

static luauc_opcode_t __luauc_arithmetic_opcode(luauc_binary_op_t op)
{
    switch (op)
    {
    case LUAUC_BINARY_ADD:
        return LOP_ADD;
    case LUAUC_BINARY_SUB:
        return LOP_SUB;
    case LUAUC_BINARY_MUL:
        return LOP_MUL;
    case LUAUC_BINARY_DIV:
        return LOP_DIV;
    case LUAUC_BINARY_FLOOR_DIV:
        return LOP_IDIV;
    case LUAUC_BINARY_MOD:
        return LOP_MOD;
    case LUAUC_BINARY_POW:
        return LOP_POW;
    default:
        return LOP_NOP;
    }
}

static luauc_opcode_t __luauc_compare_opcode(luauc_binary_op_t op)
{
    switch (op)
    {
    case LUAUC_BINARY_COMPARE_NE:
        return LOP_JUMPIFNOTEQ;
    case LUAUC_BINARY_COMPARE_EQ:
        return LOP_JUMPIFEQ;
    case LUAUC_BINARY_COMPARE_LT:
    case LUAUC_BINARY_COMPARE_GT:
        return LOP_JUMPIFLT;
    case LUAUC_BINARY_COMPARE_LE:
    case LUAUC_BINARY_COMPARE_GE:
        return LOP_JUMPIFLE;
    default:
        return LOP_NOP;
    }
}

static void __luauc_compile_binary(
    luauc_compiler_t* compiler, luauc_ast_expr_t* expression, uint8_t target
)
{
    luauc_binary_op_t op = expression->value.binary.op;
    luauc_ast_expr_t* left = expression->value.binary.left;
    luauc_ast_expr_t* right = expression->value.binary.right;
    unsigned int old_top = compiler->register_top;

    if (op <= LUAUC_BINARY_POW)
    {
        if (compiler->options.optimizationLevel >= 1 &&
            left->kind == LUAUC_EXPR_CONSTANT_NUMBER &&
            right->kind == LUAUC_EXPR_CONSTANT_NUMBER &&
            (op == LUAUC_BINARY_ADD || op == LUAUC_BINARY_SUB ||
                op == LUAUC_BINARY_MUL))
        {
            double left_value = left->value.constant_number.value;
            double right_value = right->value.constant_number.value;
            double result = op == LUAUC_BINARY_ADD ? left_value + right_value :
                op == LUAUC_BINARY_SUB ? left_value - right_value :
                left_value * right_value;
            __luauc_emit_number_constant(
                compiler, target, result, expression->location
            );
            return;
        }
        uint8_t operands = __luauc_allocate_registers(compiler, 2, expression->location);
        __luauc_compile_expression(compiler, left, operands);
        __luauc_compile_expression(compiler, right, (uint8_t)(operands + 1));
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            __luauc_arithmetic_opcode(op),
            target,
            operands,
            (uint8_t)(operands + 1)
        );
    }
    else if (op == LUAUC_BINARY_CONCAT)
    {
        uint8_t operands = __luauc_allocate_registers(compiler, 2, expression->location);
        __luauc_compile_expression(compiler, left, operands);
        __luauc_compile_expression(compiler, right, (uint8_t)(operands + 1));
        luauc_bytecode_emit_abc(
            &compiler->bytecode, LOP_CONCAT, target, operands, (uint8_t)(operands + 1)
        );
    }
    else if (op >= LUAUC_BINARY_COMPARE_NE && op <= LUAUC_BINARY_COMPARE_GE)
    {
        uint8_t operands = __luauc_allocate_registers(compiler, 2, expression->location);
        uint8_t first = operands;
        uint8_t second = (uint8_t)(operands + 1);
        size_t jump;
        size_t true_label;

        __luauc_compile_expression(compiler, left, first);
        __luauc_compile_expression(compiler, right, second);
        if (op == LUAUC_BINARY_COMPARE_GT || op == LUAUC_BINARY_COMPARE_GE)
        {
            uint8_t temporary = first;
            first = second;
            second = temporary;
        }
        jump = luauc_bytecode_emit_label(&compiler->bytecode);
        luauc_bytecode_emit_ad(&compiler->bytecode, __luauc_compare_opcode(op), first, 0);
        luauc_bytecode_emit_aux(&compiler->bytecode, second);
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_LOADB, target, 0, 1);
        true_label = luauc_bytecode_emit_label(&compiler->bytecode);
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_LOADB, target, 1, 0);
        __luauc_patch_jump(compiler, jump, true_label, expression->location);
    }
    else if (op == LUAUC_BINARY_AND || op == LUAUC_BINARY_OR)
    {
        size_t jump;
        size_t end;
        __luauc_compile_expression(compiler, left, target);
        jump = luauc_bytecode_emit_label(&compiler->bytecode);
        luauc_bytecode_emit_ad(
            &compiler->bytecode, op == LUAUC_BINARY_AND ? LOP_JUMPIFNOT : LOP_JUMPIF, target, 0
        );
        __luauc_compile_expression(compiler, right, target);
        end = luauc_bytecode_emit_label(&compiler->bytecode);
        __luauc_patch_jump(compiler, jump, end, expression->location);
    }
    else
        __luauc_compiler_raise(compiler, expression->location, "Unsupported binary expression");

    compiler->register_top = old_top;
}

static uint8_t __luauc_encode_hash_size(unsigned int hash_size)
{
    unsigned int logarithm = 0;
    unsigned int power = 1;
    if (hash_size == 0)
        return 0;
    while (power < hash_size)
    {
        power <<= 1;
        logarithm++;
    }
    return (uint8_t)(logarithm + 1);
}

static int __luauc_number_index(const luauc_ast_expr_t* expression, uint8_t* result)
{
    double value;
    if (expression == NULL || expression->kind != LUAUC_EXPR_CONSTANT_NUMBER)
        return 0;
    value = expression->value.constant_number.value;
    if (value < 1 || value > 256 || value != (double)(int)value)
        return 0;
    *result = (uint8_t)((int)value - 1);
    return 1;
}

static void __luauc_compile_table(
    luauc_compiler_t* compiler, luauc_ast_expr_t* expression, uint8_t target
)
{
    luauc_ast_table_item_t* items = (luauc_ast_table_item_t*)expression->value.table.items.data;
    size_t count = expression->value.table.items.size;
    unsigned int array_size = 0;
    unsigned int hash_size = 0;
    unsigned int array_index = 1;
    size_t index;

    for (index = 0; index < count; ++index)
    {
        if (items[index].kind == LUAUC_TABLE_ITEM_LIST)
            array_size++;
        else
            hash_size++;
    }

    luauc_bytecode_emit_abc(
        &compiler->bytecode, LOP_NEWTABLE, target, __luauc_encode_hash_size(hash_size), 0
    );
    luauc_bytecode_emit_aux(&compiler->bytecode, array_size);

    index = 0;
    while (index < count)
    {
        if (items[index].kind == LUAUC_TABLE_ITEM_LIST)
        {
            size_t run = 0;
            size_t remaining;
            unsigned int old_top = compiler->register_top;
            uint8_t values;
            int multret = 0;

            while (index + run < count && items[index + run].kind == LUAUC_TABLE_ITEM_LIST &&
                   run < 16)
                run++;
            values = __luauc_allocate_registers(
                compiler, (unsigned int)run, expression->location
            );
            for (remaining = 0; remaining < run; ++remaining)
            {
                luauc_ast_expr_t* value = items[index + remaining].value;
                if (index + remaining + 1 == count && __luauc_expression_is_multret(value))
                {
                    __luauc_compile_expression_n(
                        compiler, value, (uint8_t)(values + remaining), 0
                    );
                    multret = 1;
                }
                else
                    __luauc_compile_expression(
                        compiler, value, (uint8_t)(values + remaining)
                    );
            }
            luauc_bytecode_emit_abc(
                &compiler->bytecode,
                LOP_SETLIST,
                target,
                values,
                multret ? 0 : (uint8_t)(run + 1)
            );
            luauc_bytecode_emit_aux(&compiler->bytecode, array_index);
            array_index += (unsigned int)run;
            compiler->register_top = old_top;
            index += run;
        }
        else
        {
            unsigned int old_top = compiler->register_top;
            uint8_t value = __luauc_allocate_registers(compiler, 1, items[index].value->location);
            __luauc_compile_expression(compiler, items[index].value, value);
            if (items[index].key->kind == LUAUC_EXPR_CONSTANT_STRING)
            {
                luauc_string_ref_t key = __luauc_array_ref(
                    items[index].key->value.constant_string.value
                );
                int32_t constant = __luauc_add_string(compiler, key, items[index].key->location);
                luauc_bytecode_emit_abc(
                    &compiler->bytecode,
                    LOP_SETTABLEKS,
                    value,
                    target,
                    (uint8_t)luauc_bytecode_get_string_hash(key)
                );
                luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
            }
            else
            {
                uint8_t number;
                if (__luauc_number_index(items[index].key, &number))
                    luauc_bytecode_emit_abc(
                        &compiler->bytecode, LOP_SETTABLEN, value, target, number
                    );
                else
                {
                    uint8_t key =
                        __luauc_allocate_registers(compiler, 1, items[index].key->location);
                    __luauc_compile_expression(compiler, items[index].key, key);
                    luauc_bytecode_emit_abc(
                        &compiler->bytecode, LOP_SETTABLE, value, target, key
                    );
                }
            }
            compiler->register_top = old_top;
            index++;
        }
    }
}

static luauc_string_ref_t __luauc_build_interpolation_format(
    luauc_compiler_t* compiler, luauc_ast_expr_t* expression
)
{
    luauc_array_t* strings = (luauc_array_t*)expression->value.interpolated_string.strings.data;
    size_t string_count = expression->value.interpolated_string.strings.size;
    luauc_buffer_t buffer;
    luauc_owned_string_t owned;
    size_t index;

    luauc_buffer_init(&buffer, compiler->allocator);
    for (index = 0; index < string_count; ++index)
    {
        size_t character;
        const char* data = (const char*)strings[index].data;
        for (character = 0; character < strings[index].size; ++character)
        {
            __luauc_compiler_check(
                compiler,
                luauc_buffer_append_byte(&buffer, (uint8_t)data[character]),
                expression->location
            );
            if (data[character] == '%')
                __luauc_compiler_check(
                    compiler,
                    luauc_buffer_append_byte(&buffer, (uint8_t)'%'),
                    expression->location
                );
        }
        if (index + 1 < string_count)
            __luauc_compiler_check(
                compiler,
                luauc_buffer_append(&buffer, "%*", 2),
                expression->location
            );
    }
    owned.data = (char*)luauc_buffer_release(&buffer, &owned.size);
    __luauc_compiler_check(
        compiler,
        owned.data != NULL || owned.size == 0,
        expression->location
    );
    if (owned.data == NULL)
    {
        owned.data = (char*)compiler->allocator.reallocate(
            compiler->allocator.context, NULL, 0, 1
        );
        __luauc_compiler_check(compiler, owned.data != NULL, expression->location);
    }
    __luauc_compiler_check(
        compiler,
        luauc_vector_push(&compiler->owned_strings, &owned) != NULL,
        expression->location
    );
    {
        luauc_string_ref_t result = {owned.data, owned.size};
        return result;
    }
}

static void __luauc_compile_interpolation(
    luauc_compiler_t* compiler, luauc_ast_expr_t* expression, uint8_t target
)
{
    luauc_ast_expr_t** values =
        (luauc_ast_expr_t**)expression->value.interpolated_string.expressions.data;
    size_t count = expression->value.interpolated_string.expressions.size;
    unsigned int old_top = compiler->register_top;
    uint8_t base = __luauc_allocate_registers(
        compiler, (unsigned int)(count + 2), expression->location
    );
    luauc_string_ref_t format = __luauc_build_interpolation_format(compiler, expression);
    int32_t format_constant = __luauc_add_string(compiler, format, expression->location);
    luauc_string_ref_t method = {"format", 6};
    int32_t method_constant = __luauc_add_string(compiler, method, expression->location);
    size_t index;

    __luauc_emit_load_constant(compiler, base, format_constant);
    for (index = 0; index < count; ++index)
        __luauc_compile_expression(compiler, values[index], (uint8_t)(base + 2 + index));
    luauc_bytecode_emit_abc(
        &compiler->bytecode,
        LOP_NAMECALL,
        base,
        base,
        (uint8_t)luauc_bytecode_get_string_hash(method)
    );
    luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)method_constant);
    luauc_bytecode_emit_abc(
        &compiler->bytecode, LOP_CALL, base, (uint8_t)(count + 2), 2
    );
    if (base != target)
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_MOVE, target, base, 0);
    compiler->register_top = old_top;
}

static void __luauc_compile_closure(
    luauc_compiler_t* compiler, luauc_ast_expr_t* expression, uint8_t target
)
{
    luauc_compiler_function_t* function = __luauc_find_function(compiler, expression);
    int16_t child;
    int32_t shared = -1;
    size_t index;

    if (function == NULL || function->id == UINT32_MAX)
        __luauc_compiler_raise(compiler, expression->location, "Internal function ordering error");
    child = luauc_bytecode_add_child_function(&compiler->bytecode, function->id);
    if (child < 0)
        __luauc_compiler_raise(
            compiler, expression->location, "Exceeded closure limit; simplify the code to compile"
        );
    if (compiler->options.optimizationLevel >= 1 && function->upvalues.size == 0)
        shared = luauc_bytecode_add_constant_closure(
            &compiler->bytecode, function->id
        );
    if (shared >= 0 && shared < 32768)
        luauc_bytecode_emit_ad(
            &compiler->bytecode, LOP_DUPCLOSURE, target, (int16_t)shared
        );
    else
        luauc_bytecode_emit_ad(&compiler->bytecode, LOP_NEWCLOSURE, target, child);

    for (index = 0; index < function->upvalues.size; ++index)
    {
        luauc_ast_local_t* local =
            *(luauc_ast_local_t**)luauc_vector_at(&function->upvalues, index);
        luauc_compiler_local_t* entry = __luauc_find_local(compiler, local);
        if (entry != NULL)
        {
            entry->captured = entry->written;
            luauc_bytecode_emit_abc(
                &compiler->bytecode,
                LOP_CAPTURE,
                entry->written ? LCT_REF : LCT_VAL,
                entry->reg,
                0
            );
        }
        else
        {
            uint8_t upvalue = __luauc_get_upvalue(compiler, local);
            luauc_bytecode_emit_abc(
                &compiler->bytecode, LOP_CAPTURE, LCT_UPVAL, upvalue, 0
            );
        }
    }
}

static void __luauc_compile_expression(luauc_compiler_t* compiler, luauc_ast_expr_t* expression, uint8_t target)
{
    unsigned int old_top;
    int32_t constant;

    if (expression == NULL)
        __luauc_compiler_raise(compiler, (luauc_location_t){{0, 0}, {0, 0}}, "Missing expression");
    __luauc_set_line(compiler, expression->location);
    if (compiler->options.coverageLevel >= 2)
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_COVERAGE, 0, 0, 0);

    switch (expression->kind)
    {
    case LUAUC_EXPR_GROUP:
        __luauc_compile_expression(compiler, expression->value.group.expression, target);
        break;
    case LUAUC_EXPR_CONSTANT_NIL:
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_LOADNIL, target, 0, 0);
        break;
    case LUAUC_EXPR_CONSTANT_BOOL:
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_LOADB,
            target,
            (uint8_t)(expression->value.constant_bool.value != 0),
            0
        );
        break;
    case LUAUC_EXPR_CONSTANT_NUMBER:
        __luauc_emit_number_constant(
            compiler,
            target,
            expression->value.constant_number.value,
            expression->location
        );
        break;
    case LUAUC_EXPR_CONSTANT_INTEGER:
        constant = luauc_bytecode_add_constant_integer(
            &compiler->bytecode, expression->value.constant_integer.value
        );
        if (constant < 0)
            __luauc_compiler_raise(
                compiler, expression->location, "Exceeded constant limit; simplify the code to compile"
            );
        __luauc_emit_load_constant(compiler, target, constant);
        break;
    case LUAUC_EXPR_CONSTANT_STRING:
        constant = __luauc_add_string(
            compiler, __luauc_array_ref(expression->value.constant_string.value), expression->location
        );
        __luauc_emit_load_constant(compiler, target, constant);
        break;
    case LUAUC_EXPR_LOCAL:
    {
        luauc_compiler_local_t* local = __luauc_find_local(
            compiler, expression->value.local.local
        );
        if (local != NULL)
        {
            if (local->reg != target)
                luauc_bytecode_emit_abc(
                    &compiler->bytecode, LOP_MOVE, target, local->reg, 0
                );
        }
        else
            luauc_bytecode_emit_abc(
                &compiler->bytecode,
                LOP_GETUPVAL,
                target,
                __luauc_get_upvalue(compiler, expression->value.local.local),
                0
            );
        break;
    }
    case LUAUC_EXPR_GLOBAL:
    {
        luauc_string_ref_t name = __luauc_name_ref(expression->value.global.name);
        if (__luauc_try_compile_import(compiler, expression, target))
            break;
        constant = __luauc_add_string(compiler, name, expression->location);
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_GETGLOBAL,
            target,
            0,
            (uint8_t)luauc_bytecode_get_string_hash(name)
        );
        luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
        break;
    }
    case LUAUC_EXPR_VARARGS:
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_GETVARARGS, target, 2, 0);
        break;
    case LUAUC_EXPR_CALL:
        __luauc_compile_call(compiler, expression, target, 1, 0);
        break;
    case LUAUC_EXPR_INDEX_NAME:
    {
        luauc_string_ref_t name = __luauc_name_ref(expression->value.index_name.index);
        if (__luauc_try_compile_known_library_member(compiler, expression, name, target))
            break;
        if (__luauc_try_compile_import(compiler, expression, target))
            break;
        old_top = compiler->register_top;
        {
            uint8_t table = target;
            __luauc_compile_expression(
                compiler, expression->value.index_name.expression, table
            );
            constant = __luauc_add_string(compiler, name, expression->location);
            luauc_bytecode_emit_abc(
                &compiler->bytecode,
                LOP_GETTABLEKS,
                target,
                table,
                (uint8_t)luauc_bytecode_get_string_hash(name)
            );
            luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
        }
        compiler->register_top = old_top;
        break;
    }
    case LUAUC_EXPR_INDEX_EXPR:
    {
        uint8_t number;
        old_top = compiler->register_top;
        {
            uint8_t table = target;
            __luauc_compile_expression(
                compiler, expression->value.index_expr.expression, table
            );
            if (__luauc_number_index(expression->value.index_expr.index, &number))
                luauc_bytecode_emit_abc(
                    &compiler->bytecode, LOP_GETTABLEN, target, table, number
                );
            else if (expression->value.index_expr.index->kind == LUAUC_EXPR_CONSTANT_STRING)
            {
                luauc_string_ref_t name = __luauc_array_ref(
                    expression->value.index_expr.index->value.constant_string.value
                );
                constant = __luauc_add_string(
                    compiler, name, expression->value.index_expr.index->location
                );
                luauc_bytecode_emit_abc(
                    &compiler->bytecode,
                    LOP_GETTABLEKS,
                    target,
                    table,
                    (uint8_t)luauc_bytecode_get_string_hash(name)
                );
                luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
            }
            else
            {
                uint8_t index = __luauc_allocate_registers(
                    compiler, 1, expression->value.index_expr.index->location
                );
                __luauc_compile_expression(
                    compiler, expression->value.index_expr.index, index
                );
                luauc_bytecode_emit_abc(
                    &compiler->bytecode, LOP_GETTABLE, target, table, index
                );
            }
        }
        compiler->register_top = old_top;
        break;
    }
    case LUAUC_EXPR_FUNCTION:
        __luauc_compile_closure(compiler, expression, target);
        break;
    case LUAUC_EXPR_TABLE:
        __luauc_compile_table(compiler, expression, target);
        break;
    case LUAUC_EXPR_UNARY:
        if (compiler->options.optimizationLevel >= 1 &&
            expression->value.unary.op == LUAUC_UNARY_MINUS &&
            expression->value.unary.expression->kind == LUAUC_EXPR_CONSTANT_NUMBER)
        {
            __luauc_emit_number_constant(
                compiler,
                target,
                -expression->value.unary.expression->value.constant_number.value,
                expression->location
            );
            break;
        }
        if (expression->value.unary.op == LUAUC_UNARY_MINUS &&
            expression->value.unary.expression->kind == LUAUC_EXPR_CONSTANT_INTEGER)
        {
            uint64_t value =
                (uint64_t)expression->value.unary.expression->value.constant_integer.value;
            constant = luauc_bytecode_add_constant_integer(
                &compiler->bytecode, (int64_t)(~value + UINT64_C(1))
            );
            if (constant < 0)
                __luauc_compiler_raise(
                    compiler,
                    expression->location,
                    "Exceeded constant limit; simplify the code to compile"
                );
            __luauc_emit_load_constant(compiler, target, constant);
            break;
        }
        old_top = compiler->register_top;
        {
            uint8_t operand = __luauc_allocate_registers(compiler, 1, expression->location);
            luauc_opcode_t op = expression->value.unary.op == LUAUC_UNARY_NOT ? LOP_NOT :
                expression->value.unary.op == LUAUC_UNARY_MINUS ? LOP_MINUS : LOP_LENGTH;
            __luauc_compile_expression(compiler, expression->value.unary.expression, operand);
            luauc_bytecode_emit_abc(&compiler->bytecode, op, target, operand, 0);
        }
        compiler->register_top = old_top;
        break;
    case LUAUC_EXPR_BINARY:
        __luauc_compile_binary(compiler, expression, target);
        break;
    case LUAUC_EXPR_TYPE_ASSERTION:
        __luauc_compile_expression(
            compiler, expression->value.type_assertion.expression, target
        );
        break;
    case LUAUC_EXPR_IF_ELSE:
    {
        size_t else_jump = __luauc_emit_condition_jump(
            compiler, expression->value.if_else.condition, 0
        );
        size_t end_jump;
        size_t else_label;
        size_t end_label;
        __luauc_compile_expression(
            compiler, expression->value.if_else.true_expression, target
        );
        end_jump = luauc_bytecode_emit_label(&compiler->bytecode);
        luauc_bytecode_emit_ad(&compiler->bytecode, LOP_JUMP, 0, 0);
        else_label = luauc_bytecode_emit_label(&compiler->bytecode);
        __luauc_compile_expression(
            compiler, expression->value.if_else.false_expression, target
        );
        end_label = luauc_bytecode_emit_label(&compiler->bytecode);
        __luauc_patch_jump(compiler, else_jump, else_label, expression->location);
        __luauc_patch_jump(compiler, end_jump, end_label, expression->location);
        break;
    }
    case LUAUC_EXPR_INTERP_STRING:
        __luauc_compile_interpolation(compiler, expression, target);
        break;
    case LUAUC_EXPR_INSTANTIATE:
        __luauc_compile_expression(compiler, expression->value.instantiate.expression, target);
        break;
    default:
        __luauc_compiler_raise(compiler, expression->location, "Unsupported expression");
    }
}

static void __luauc_emit_close_upvalues(luauc_compiler_t* compiler, size_t start)
{
    size_t index;
    int captured = 0;
    uint8_t first = UINT8_MAX;
    for (index = start; index < compiler->locals.size; ++index)
    {
        luauc_compiler_local_t* entry =
            (luauc_compiler_local_t*)luauc_vector_at(&compiler->locals, index);
        if (entry->captured)
        {
            captured = 1;
            if (entry->reg < first)
                first = entry->reg;
        }
    }
    if (captured)
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_CLOSEUPVALS, first, 0, 0);
}

static luauc_lvalue_t __luauc_compile_lvalue(luauc_compiler_t* compiler, luauc_ast_expr_t* expression)
{
    luauc_lvalue_t result;
    memset(&result, 0, sizeof(result));
    result.location = expression->location;

    if (expression->kind == LUAUC_EXPR_LOCAL)
    {
        luauc_compiler_local_t* local =
            __luauc_find_local(compiler, expression->value.local.local);
        if (local != NULL)
        {
            result.kind = LUAUC_LVALUE_LOCAL;
            result.reg = local->reg;
        }
        else
        {
            result.kind = LUAUC_LVALUE_UPVALUE;
            result.upvalue = __luauc_get_upvalue(compiler, expression->value.local.local);
        }
    }
    else if (expression->kind == LUAUC_EXPR_GLOBAL)
    {
        result.kind = LUAUC_LVALUE_GLOBAL;
        result.name = __luauc_name_ref(expression->value.global.name);
    }
    else if (expression->kind == LUAUC_EXPR_INDEX_NAME)
    {
        result.kind = LUAUC_LVALUE_INDEX_NAME;
        result.reg = __luauc_allocate_registers(compiler, 1, expression->location);
        result.name = __luauc_name_ref(expression->value.index_name.index);
        __luauc_compile_expression(
            compiler, expression->value.index_name.expression, result.reg
        );
    }
    else if (expression->kind == LUAUC_EXPR_INDEX_EXPR)
    {
        uint8_t number;
        result.reg = __luauc_allocate_registers(compiler, 1, expression->location);
        __luauc_compile_expression(
            compiler, expression->value.index_expr.expression, result.reg
        );
        if (__luauc_number_index(expression->value.index_expr.index, &number))
        {
            result.kind = LUAUC_LVALUE_INDEX_NUMBER;
            result.number = number;
        }
        else if (expression->value.index_expr.index->kind == LUAUC_EXPR_CONSTANT_STRING)
        {
            result.kind = LUAUC_LVALUE_INDEX_NAME;
            result.name = __luauc_array_ref(
                expression->value.index_expr.index->value.constant_string.value
            );
        }
        else
        {
            result.kind = LUAUC_LVALUE_INDEX_EXPR;
            result.index = __luauc_allocate_registers(
                compiler, 1, expression->value.index_expr.index->location
            );
            __luauc_compile_expression(
                compiler, expression->value.index_expr.index, result.index
            );
        }
    }
    else
        __luauc_compiler_raise(compiler, expression->location, "Invalid assignment target");
    return result;
}

static void __luauc_read_lvalue(
    luauc_compiler_t* compiler, const luauc_lvalue_t* value, uint8_t target
)
{
    int32_t constant;
    switch (value->kind)
    {
    case LUAUC_LVALUE_LOCAL:
        if (target != value->reg)
            luauc_bytecode_emit_abc(
                &compiler->bytecode, LOP_MOVE, target, value->reg, 0
            );
        break;
    case LUAUC_LVALUE_UPVALUE:
        luauc_bytecode_emit_abc(
            &compiler->bytecode, LOP_GETUPVAL, target, value->upvalue, 0
        );
        break;
    case LUAUC_LVALUE_GLOBAL:
        constant = __luauc_add_string(compiler, value->name, value->location);
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_GETGLOBAL,
            target,
            0,
            (uint8_t)luauc_bytecode_get_string_hash(value->name)
        );
        luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
        break;
    case LUAUC_LVALUE_INDEX_NAME:
        constant = __luauc_add_string(compiler, value->name, value->location);
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_GETTABLEKS,
            target,
            value->reg,
            (uint8_t)luauc_bytecode_get_string_hash(value->name)
        );
        luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
        break;
    case LUAUC_LVALUE_INDEX_NUMBER:
        luauc_bytecode_emit_abc(
            &compiler->bytecode, LOP_GETTABLEN, target, value->reg, value->number
        );
        break;
    case LUAUC_LVALUE_INDEX_EXPR:
        luauc_bytecode_emit_abc(
            &compiler->bytecode, LOP_GETTABLE, target, value->reg, value->index
        );
        break;
    }
}

static void __luauc_write_lvalue(
    luauc_compiler_t* compiler, const luauc_lvalue_t* value, uint8_t source
)
{
    int32_t constant;
    switch (value->kind)
    {
    case LUAUC_LVALUE_LOCAL:
        if (source != value->reg)
            luauc_bytecode_emit_abc(
                &compiler->bytecode, LOP_MOVE, value->reg, source, 0
            );
        break;
    case LUAUC_LVALUE_UPVALUE:
        luauc_bytecode_emit_abc(
            &compiler->bytecode, LOP_SETUPVAL, source, value->upvalue, 0
        );
        break;
    case LUAUC_LVALUE_GLOBAL:
        constant = __luauc_add_string(compiler, value->name, value->location);
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_SETGLOBAL,
            source,
            0,
            (uint8_t)luauc_bytecode_get_string_hash(value->name)
        );
        luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
        break;
    case LUAUC_LVALUE_INDEX_NAME:
        constant = __luauc_add_string(compiler, value->name, value->location);
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_SETTABLEKS,
            source,
            value->reg,
            (uint8_t)luauc_bytecode_get_string_hash(value->name)
        );
        luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)constant);
        break;
    case LUAUC_LVALUE_INDEX_NUMBER:
        luauc_bytecode_emit_abc(
            &compiler->bytecode, LOP_SETTABLEN, source, value->reg, value->number
        );
        break;
    case LUAUC_LVALUE_INDEX_EXPR:
        luauc_bytecode_emit_abc(
            &compiler->bytecode, LOP_SETTABLE, source, value->reg, value->index
        );
        break;
    }
}

static void __luauc_patch_loop_jumps(
    luauc_compiler_t* compiler,
    size_t start,
    size_t break_target,
    size_t continue_target,
    luauc_location_t location
)
{
    size_t index;
    for (index = start; index < compiler->loop_jumps.size; ++index)
    {
        luauc_loop_jump_t* jump =
            (luauc_loop_jump_t*)luauc_vector_at(&compiler->loop_jumps, index);
        __luauc_patch_jump(
            compiler,
            jump->label,
            jump->kind == LUAUC_LOOP_BREAK ? break_target : continue_target,
            location
        );
    }
    compiler->loop_jumps.size = start;
}

static void __luauc_compile_block(luauc_compiler_t* compiler, luauc_ast_stat_t* block, int scoped)
{
    size_t local_start = compiler->locals.size;
    unsigned int register_start = compiler->register_top;
    size_t index;
    luauc_ast_stat_t** statements = (luauc_ast_stat_t**)block->value.block.body.data;

    for (index = 0; index < block->value.block.body.size; ++index)
    {
        __luauc_compile_statement(compiler, statements[index]);
        if (__luauc_statement_terminates(statements[index]))
            break;
    }
    if (scoped)
    {
        __luauc_close_locals(compiler, local_start);
        __luauc_pop_locals(compiler, local_start);
        compiler->register_top = register_start;
    }
}

static void __luauc_compile_return(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    luauc_ast_expr_t** expressions =
        (luauc_ast_expr_t**)statement->value.return_statement.expressions.data;
    size_t count = statement->value.return_statement.expressions.size;
    unsigned int old_top = compiler->register_top;
    uint8_t first = 0;
    int multret = 0;
    size_t index;

    if (count >= 255)
        __luauc_compiler_raise(
            compiler,
            statement->location,
            "Exceeded return count limit; simplify the code to compile"
        );
    if (count != 0)
    {
        luauc_compiler_local_t* direct_local = count == 1 &&
                expressions[0]->kind == LUAUC_EXPR_LOCAL ?
            __luauc_find_local(compiler, expressions[0]->value.local.local) : NULL;
        if (direct_local != NULL)
            first = direct_local->reg;
        else
        {
            first = __luauc_allocate_registers(
                compiler, (unsigned int)count, statement->location
            );
            for (index = 0; index < count; ++index)
            {
                if (index + 1 == count && __luauc_expression_is_multret(expressions[index]))
                {
                    __luauc_compile_expression_n(
                        compiler, expressions[index], (uint8_t)(first + index), 0
                    );
                    multret = 1;
                }
                else
                    __luauc_compile_expression(
                        compiler, expressions[index], (uint8_t)(first + index)
                    );
            }
        }
    }
    __luauc_emit_close_upvalues(compiler, 0);
    luauc_bytecode_emit_abc(
        &compiler->bytecode,
        LOP_RETURN,
        first,
        multret ? 0 : (uint8_t)(count + 1),
        0
    );
    compiler->register_top = old_top;
}

static void __luauc_compile_local(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    luauc_ast_local_t** locals = (luauc_ast_local_t**)statement->value.local.variables.data;
    size_t count = statement->value.local.variables.size;
    uint8_t registers = __luauc_allocate_registers(
        compiler, (unsigned int)count, statement->location
    );
    uint32_t startpc = luauc_bytecode_get_debug_pc(&compiler->bytecode);
    size_t index;

    __luauc_compile_expression_list(
        compiler, statement->value.local.values, registers, (uint8_t)count
    );
    for (index = 0; index < count; ++index)
        __luauc_push_local(
            compiler, locals[index], (uint8_t)(registers + index), startpc
        );
}

static void __luauc_compile_assignment(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    luauc_ast_expr_t** variables = (luauc_ast_expr_t**)statement->value.assign.variables.data;
    size_t count = statement->value.assign.variables.size;
    unsigned int old_top = compiler->register_top;
    luauc_vector_t lvalues;
    uint8_t values;
    size_t index;

    luauc_vector_init(&lvalues, sizeof(luauc_lvalue_t), compiler->allocator);
    for (index = 0; index < count; ++index)
    {
        luauc_lvalue_t value = __luauc_compile_lvalue(compiler, variables[index]);
        if (luauc_vector_push(&lvalues, &value) == NULL)
        {
            luauc_vector_destroy(&lvalues);
            __luauc_compiler_raise(compiler, statement->location, "Out of memory");
        }
    }
    values = __luauc_allocate_registers(compiler, (unsigned int)count, statement->location);
    __luauc_compile_expression_list(
        compiler, statement->value.assign.values, values, (uint8_t)count
    );
    for (index = 0; index < count; ++index)
        __luauc_write_lvalue(
            compiler,
            (const luauc_lvalue_t*)luauc_vector_at_const(&lvalues, index),
            (uint8_t)(values + index)
        );
    luauc_vector_destroy(&lvalues);
    compiler->register_top = old_top;
}

static void __luauc_compile_compound_assignment(
    luauc_compiler_t* compiler, luauc_ast_stat_t* statement
)
{
    unsigned int old_top = compiler->register_top;
    luauc_lvalue_t lvalue =
        __luauc_compile_lvalue(compiler, statement->value.compound_assign.variable);
    uint8_t operands = __luauc_allocate_registers(compiler, 2, statement->location);
    luauc_binary_op_t op = statement->value.compound_assign.op;

    __luauc_read_lvalue(compiler, &lvalue, operands);
    if (op == LUAUC_BINARY_CONCAT)
    {
        __luauc_compile_expression(
            compiler, statement->value.compound_assign.value, (uint8_t)(operands + 1)
        );
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_CONCAT,
            operands,
            operands,
            (uint8_t)(operands + 1)
        );
    }
    else
    {
        __luauc_compile_expression(
            compiler, statement->value.compound_assign.value, (uint8_t)(operands + 1)
        );
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            __luauc_arithmetic_opcode(op),
            operands,
            operands,
            (uint8_t)(operands + 1)
        );
    }
    __luauc_write_lvalue(compiler, &lvalue, operands);
    compiler->register_top = old_top;
}

static void __luauc_compile_if(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    size_t else_jump = __luauc_emit_condition_jump(
        compiler, statement->value.if_statement.condition, 0
    );
    size_t end_jump = SIZE_MAX;
    size_t else_label;
    size_t end_label;

    __luauc_compile_statement(compiler, statement->value.if_statement.then_body);
    if (statement->value.if_statement.else_body != NULL &&
        !__luauc_statement_terminates(statement->value.if_statement.then_body))
    {
        end_jump = luauc_bytecode_emit_label(&compiler->bytecode);
        luauc_bytecode_emit_ad(&compiler->bytecode, LOP_JUMP, 0, 0);
    }
    else_label = luauc_bytecode_emit_label(&compiler->bytecode);
    if (statement->value.if_statement.else_body != NULL)
        __luauc_compile_statement(compiler, statement->value.if_statement.else_body);
    end_label = luauc_bytecode_emit_label(&compiler->bytecode);
    __luauc_patch_jump(compiler, else_jump, else_label, statement->location);
    if (end_jump != SIZE_MAX)
        __luauc_patch_jump(compiler, end_jump, end_label, statement->location);
}

static void __luauc_compile_while(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    luauc_compiler_loop_t loop;
    size_t loop_label = luauc_bytecode_emit_label(&compiler->bytecode);
    size_t exit_jump = __luauc_emit_condition_jump(
        compiler, statement->value.while_statement.condition, 0
    );
    size_t continue_label;
    size_t back_jump;
    size_t end_label;

    loop.jump_start = compiler->loop_jumps.size;
    loop.local_start = compiler->locals.size;
    loop.continue_local_start = loop.local_start;
    __luauc_compiler_check(
        compiler,
        luauc_vector_push(&compiler->loops, &loop) != NULL,
        statement->location
    );
    __luauc_compile_statement(compiler, statement->value.while_statement.body);
    continue_label = luauc_bytecode_emit_label(&compiler->bytecode);
    back_jump = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_bytecode_emit_ad(&compiler->bytecode, LOP_JUMPBACK, 0, 0);
    end_label = luauc_bytecode_emit_label(&compiler->bytecode);
    __luauc_patch_jump(compiler, exit_jump, end_label, statement->location);
    __luauc_patch_jump(compiler, back_jump, loop_label, statement->location);
    __luauc_patch_loop_jumps(
        compiler, loop.jump_start, end_label, continue_label, statement->location
    );
    compiler->loops.size--;
}

static void __luauc_compile_repeat(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    luauc_ast_stat_t* body = statement->value.repeat_statement.body;
    luauc_ast_stat_t** statements = (luauc_ast_stat_t**)body->value.block.body.data;
    size_t local_start = compiler->locals.size;
    unsigned int register_start = compiler->register_top;
    size_t loop_label = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_compiler_loop_t loop;
    size_t index;
    size_t continue_label;
    size_t back_jump;
    size_t end_label;

    loop.jump_start = compiler->loop_jumps.size;
    loop.local_start = local_start;
    loop.continue_local_start = local_start;
    __luauc_compiler_check(
        compiler,
        luauc_vector_push(&compiler->loops, &loop) != NULL,
        statement->location
    );
    for (index = 0; index < body->value.block.body.size; ++index)
    {
        ((luauc_compiler_loop_t*)luauc_vector_at(
             &compiler->loops, compiler->loops.size - 1
         ))->continue_local_start = compiler->locals.size;
        __luauc_compile_statement(compiler, statements[index]);
    }
    continue_label = luauc_bytecode_emit_label(&compiler->bytecode);
    {
        size_t done_jump = __luauc_emit_condition_jump(
            compiler, statement->value.repeat_statement.condition, 1
        );
        __luauc_emit_close_upvalues(compiler, local_start);
        back_jump = luauc_bytecode_emit_label(&compiler->bytecode);
        luauc_bytecode_emit_ad(&compiler->bytecode, LOP_JUMPBACK, 0, 0);
        end_label = luauc_bytecode_emit_label(&compiler->bytecode);
        __luauc_patch_jump(compiler, done_jump, end_label, statement->location);
    }
    __luauc_close_locals(compiler, local_start);
    __luauc_pop_locals(compiler, local_start);
    compiler->register_top = register_start;
    __luauc_patch_jump(compiler, back_jump, loop_label, statement->location);
    __luauc_patch_loop_jumps(
        compiler, loop.jump_start, end_label, continue_label, statement->location
    );
    compiler->loops.size--;
}

static void __luauc_compile_numeric_for(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    unsigned int old_top = compiler->register_top;
    size_t local_start = compiler->locals.size;
    uint8_t registers = __luauc_allocate_registers(compiler, 4, statement->location);
    uint8_t variable = (uint8_t)(registers + 3);
    uint32_t variable_pc = luauc_bytecode_get_debug_pc(&compiler->bytecode);
    size_t prep_jump;
    size_t loop_label;
    size_t continue_label;
    size_t back_jump;
    size_t end_label;
    luauc_compiler_loop_t loop;

    __luauc_compile_expression(
        compiler, statement->value.for_statement.from, (uint8_t)(registers + 2)
    );
    __luauc_compile_expression(
        compiler, statement->value.for_statement.to, registers
    );
    if (statement->value.for_statement.step != NULL)
        __luauc_compile_expression(
            compiler, statement->value.for_statement.step, (uint8_t)(registers + 1)
        );
    else
        luauc_bytecode_emit_ad(
            &compiler->bytecode, LOP_LOADN, (uint8_t)(registers + 1), 1
        );
    prep_jump = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_bytecode_emit_ad(&compiler->bytecode, LOP_FORNPREP, registers, 0);
    loop_label = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_bytecode_emit_abc(
        &compiler->bytecode, LOP_MOVE, variable, (uint8_t)(registers + 2), 0
    );
    __luauc_push_local(
        compiler, statement->value.for_statement.variable, variable, variable_pc
    );

    loop.jump_start = compiler->loop_jumps.size;
    loop.local_start = local_start;
    loop.continue_local_start = local_start;
    __luauc_compiler_check(
        compiler,
        luauc_vector_push(&compiler->loops, &loop) != NULL,
        statement->location
    );
    __luauc_compile_statement(compiler, statement->value.for_statement.body);
    __luauc_close_locals(compiler, local_start);
    __luauc_pop_locals(compiler, local_start);
    continue_label = luauc_bytecode_emit_label(&compiler->bytecode);
    back_jump = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_bytecode_emit_ad(&compiler->bytecode, LOP_FORNLOOP, registers, 0);
    end_label = luauc_bytecode_emit_label(&compiler->bytecode);
    __luauc_patch_jump(compiler, prep_jump, end_label, statement->location);
    __luauc_patch_jump(compiler, back_jump, loop_label, statement->location);
    __luauc_patch_loop_jumps(
        compiler, loop.jump_start, end_label, continue_label, statement->location
    );
    compiler->loops.size--;
    compiler->register_top = old_top;
}

static void __luauc_compile_generic_for(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    luauc_ast_local_t** variables = (luauc_ast_local_t**)statement->value.for_in.variables.data;
    size_t variable_count = statement->value.for_in.variables.size;
    unsigned int old_top = compiler->register_top;
    size_t local_start = compiler->locals.size;
    unsigned int reserved_variables = variable_count < 2 ? 2u : (unsigned int)variable_count;
    uint8_t registers = __luauc_allocate_registers(
        compiler, 3u + reserved_variables, statement->location
    );
    uint8_t variable_registers = (uint8_t)(registers + 3);
    uint32_t variable_pc = luauc_bytecode_get_debug_pc(&compiler->bytecode);
    size_t prep_jump;
    size_t loop_label;
    size_t continue_label;
    size_t back_jump;
    size_t end_label;
    size_t index;
    luauc_compiler_loop_t loop;

    __luauc_compile_expression_list(
        compiler, statement->value.for_in.values, registers, 3
    );
    prep_jump = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_bytecode_emit_ad(&compiler->bytecode, LOP_FORGPREP, registers, 0);
    loop_label = luauc_bytecode_emit_label(&compiler->bytecode);
    for (index = 0; index < variable_count; ++index)
        __luauc_push_local(
            compiler, variables[index], (uint8_t)(variable_registers + index), variable_pc
        );

    loop.jump_start = compiler->loop_jumps.size;
    loop.local_start = local_start;
    loop.continue_local_start = local_start;
    __luauc_compiler_check(
        compiler,
        luauc_vector_push(&compiler->loops, &loop) != NULL,
        statement->location
    );
    __luauc_compile_statement(compiler, statement->value.for_in.body);
    __luauc_close_locals(compiler, local_start);
    __luauc_pop_locals(compiler, local_start);
    continue_label = luauc_bytecode_emit_label(&compiler->bytecode);
    back_jump = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_bytecode_emit_ad(&compiler->bytecode, LOP_FORGLOOP, registers, 0);
    luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)variable_count);
    end_label = luauc_bytecode_emit_label(&compiler->bytecode);
    __luauc_patch_jump(compiler, prep_jump, back_jump, statement->location);
    __luauc_patch_jump(compiler, back_jump, loop_label, statement->location);
    __luauc_patch_loop_jumps(
        compiler, loop.jump_start, end_label, continue_label, statement->location
    );
    compiler->loops.size--;
    compiler->register_top = old_top;
}

static void __luauc_compile_function_statement(
    luauc_compiler_t* compiler, luauc_ast_stat_t* statement
)
{
    unsigned int old_top = compiler->register_top;
    luauc_lvalue_t target = __luauc_compile_lvalue(compiler, statement->value.function.name);
    uint8_t value = __luauc_allocate_registers(compiler, 1, statement->location);
    __luauc_compile_closure(compiler, statement->value.function.function, value);
    __luauc_write_lvalue(compiler, &target, value);
    compiler->register_top = old_top;
}

static void __luauc_compile_local_function(
    luauc_compiler_t* compiler, luauc_ast_stat_t* statement
)
{
    uint8_t value = __luauc_allocate_registers(compiler, 1, statement->location);
    uint32_t startpc = luauc_bytecode_get_debug_pc(&compiler->bytecode);
    __luauc_push_local(compiler, statement->value.local_function.name, value, startpc);
    __luauc_compile_closure(compiler, statement->value.local_function.function, value);
}

static void __luauc_compile_class(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    luauc_class_member_t* members =
        (luauc_class_member_t*)statement->value.class_statement.members.data;
    size_t member_count = statement->value.class_statement.members.size;
    size_t property_count = 0;
    size_t method_count = 0;
    int32_t* properties;
    int32_t* methods;
    size_t property_index = 0;
    size_t method_index = 0;
    size_t index;
    uint8_t destination = __luauc_allocate_registers(compiler, 1, statement->location);
    uint32_t startpc = luauc_bytecode_get_debug_pc(&compiler->bytecode);
    size_t aux_label;
    luauc_class_shape_t shape;
    int32_t class_constant;

    for (index = 0; index < member_count; ++index)
        if (members[index].is_method)
            method_count++;
        else
            property_count++;
    properties = property_count == 0 ? NULL : (int32_t*)compiler->allocator.reallocate(
        compiler->allocator.context, NULL, 0, property_count * sizeof(int32_t)
    );
    methods = method_count == 0 ? NULL : (int32_t*)compiler->allocator.reallocate(
        compiler->allocator.context, NULL, 0, method_count * sizeof(int32_t)
    );
    if ((property_count != 0 && properties == NULL) || (method_count != 0 && methods == NULL))
    {
        if (properties != NULL)
            compiler->allocator.reallocate(
                compiler->allocator.context, properties, property_count * sizeof(int32_t), 0
            );
        if (methods != NULL)
            compiler->allocator.reallocate(
                compiler->allocator.context, methods, method_count * sizeof(int32_t), 0
            );
        __luauc_compiler_raise(compiler, statement->location, "Out of memory");
    }

    __luauc_push_local(
        compiler, statement->value.class_statement.name, destination, startpc
    );
    luauc_bytecode_emit_ad(&compiler->bytecode, LOP_LOADKX, destination, 0);
    aux_label = luauc_bytecode_emit_label(&compiler->bytecode);
    luauc_bytecode_emit_aux(&compiler->bytecode, 0);

    for (index = 0; index < member_count; ++index)
    {
        if (members[index].is_method)
        {
            unsigned int old_top = compiler->register_top;
            uint8_t value = __luauc_allocate_registers(compiler, 1, statement->location);
            int32_t name = __luauc_add_string(
                compiler,
                __luauc_name_ref(members[index].value.method.function_name),
                members[index].value.method.name_location
            );
            __luauc_compile_closure(compiler, members[index].value.method.function, value);
            methods[method_index++] = name;
            luauc_bytecode_emit_abc(
                &compiler->bytecode, LOP_NEWCLASSMEMBER, destination, 0, value
            );
            luauc_bytecode_emit_aux(&compiler->bytecode, (uint32_t)name);
            compiler->register_top = old_top;
        }
        else
            properties[property_index++] = __luauc_add_string(
                compiler,
                __luauc_name_ref(members[index].value.property.name),
                members[index].value.property.name_location
            );
    }
    shape.class_name = __luauc_add_string(
        compiler,
        __luauc_name_ref(statement->value.class_statement.name->name),
        statement->value.class_statement.name->location
    );
    shape.property_names = properties;
    shape.property_count = property_count;
    shape.method_names = methods;
    shape.method_count = method_count;
    class_constant = luauc_bytecode_add_class_shape(&compiler->bytecode, &shape);
    if (class_constant < 0)
        __luauc_compiler_raise(
            compiler, statement->location, "Exceeded constant limit; simplify the code to compile"
        );
    luauc_bytecode_patch_aux(&compiler->bytecode, aux_label, class_constant);
    if (properties != NULL)
        compiler->allocator.reallocate(
            compiler->allocator.context, properties, property_count * sizeof(int32_t), 0
        );
    if (methods != NULL)
        compiler->allocator.reallocate(
            compiler->allocator.context, methods, method_count * sizeof(int32_t), 0
        );
}

static void __luauc_compile_statement(luauc_compiler_t* compiler, luauc_ast_stat_t* statement)
{
    __luauc_set_line(compiler, statement->location);
    if (compiler->options.coverageLevel >= 1 && statement->kind != LUAUC_STAT_BLOCK &&
        statement->kind != LUAUC_STAT_TYPE_ALIAS)
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_COVERAGE, 0, 0, 0);

    switch (statement->kind)
    {
    case LUAUC_STAT_BLOCK:
        __luauc_compile_block(compiler, statement, 1);
        break;
    case LUAUC_STAT_IF:
        __luauc_compile_if(compiler, statement);
        break;
    case LUAUC_STAT_WHILE:
        __luauc_compile_while(compiler, statement);
        break;
    case LUAUC_STAT_REPEAT:
        __luauc_compile_repeat(compiler, statement);
        break;
    case LUAUC_STAT_BREAK:
    case LUAUC_STAT_CONTINUE:
    {
        luauc_compiler_loop_t* loop;
        luauc_loop_jump_t jump;
        if (compiler->loops.size == 0)
            __luauc_compiler_raise(compiler, statement->location, "Loop control statement outside loop");
        loop = (luauc_compiler_loop_t*)luauc_vector_at(
            &compiler->loops, compiler->loops.size - 1
        );
        __luauc_emit_close_upvalues(
            compiler,
            statement->kind == LUAUC_STAT_CONTINUE ?
                loop->continue_local_start : loop->local_start
        );
        jump.kind = statement->kind == LUAUC_STAT_BREAK ? LUAUC_LOOP_BREAK : LUAUC_LOOP_CONTINUE;
        jump.label = luauc_bytecode_emit_label(&compiler->bytecode);
        luauc_bytecode_emit_ad(&compiler->bytecode, LOP_JUMP, 0, 0);
        __luauc_compiler_check(
            compiler,
            luauc_vector_push(&compiler->loop_jumps, &jump) != NULL,
            statement->location
        );
        break;
    }
    case LUAUC_STAT_RETURN:
        __luauc_compile_return(compiler, statement);
        break;
    case LUAUC_STAT_EXPR:
        if (statement->value.expression.expression->kind == LUAUC_EXPR_CALL)
            __luauc_compile_call(
                compiler, statement->value.expression.expression, 0, 0, 0
            );
        else
        {
            unsigned int old_top = compiler->register_top;
            uint8_t discard = __luauc_allocate_registers(
                compiler, 1, statement->location
            );
            __luauc_compile_expression(
                compiler, statement->value.expression.expression, discard
            );
            compiler->register_top = old_top;
        }
        break;
    case LUAUC_STAT_LOCAL:
        __luauc_compile_local(compiler, statement);
        break;
    case LUAUC_STAT_FOR:
        __luauc_compile_numeric_for(compiler, statement);
        break;
    case LUAUC_STAT_FOR_IN:
        __luauc_compile_generic_for(compiler, statement);
        break;
    case LUAUC_STAT_ASSIGN:
        __luauc_compile_assignment(compiler, statement);
        break;
    case LUAUC_STAT_COMPOUND_ASSIGN:
        __luauc_compile_compound_assignment(compiler, statement);
        break;
    case LUAUC_STAT_FUNCTION:
        __luauc_compile_function_statement(compiler, statement);
        break;
    case LUAUC_STAT_LOCAL_FUNCTION:
        __luauc_compile_local_function(compiler, statement);
        break;
    case LUAUC_STAT_TYPE_ALIAS:
    case LUAUC_STAT_TYPE_FUNCTION:
    case LUAUC_STAT_DECLARE_GLOBAL:
    case LUAUC_STAT_DECLARE_FUNCTION:
    case LUAUC_STAT_DECLARE_EXTERN_TYPE:
        break;
    case LUAUC_STAT_CLASS:
        __luauc_compile_class(compiler, statement);
        break;
    default:
        __luauc_compiler_raise(compiler, statement->location, "Unsupported statement");
    }
}

static uint8_t __luauc_function_flags(const luauc_ast_expr_t* function, int is_main)
{
    uint8_t flags = 0;
    size_t index;
    if (is_main)
        flags |= LPF_NATIVE_COLD;
    if (function != NULL)
    {
        luauc_ast_attr_t** attributes =
            (luauc_ast_attr_t**)function->value.function.attributes.data;
        for (index = 0; index < function->value.function.attributes.size; ++index)
            if (attributes[index]->type == LUAUC_ATTR_NATIVE)
                flags |= LPF_NATIVE_FUNCTION;
    }
    return flags;
}

static uint32_t __luauc_compile_function(
    luauc_compiler_t* compiler,
    luauc_compiler_function_t* function,
    luauc_ast_stat_t* main_body,
    uint8_t main_flags
)
{
    luauc_ast_expr_t* expression = function->expression;
    luauc_ast_stat_t* body =
        expression != NULL ? expression->value.function.body : main_body;
    luauc_ast_local_t** arguments = expression != NULL ?
        (luauc_ast_local_t**)expression->value.function.arguments.data : NULL;
    size_t argument_count =
        expression != NULL ? expression->value.function.arguments.size : 0;
    int has_self = expression != NULL && expression->value.function.self != NULL;
    int vararg = expression == NULL ? 1 : expression->value.function.vararg;
    uint32_t id;
    uint8_t argument_registers;
    size_t index;
    uint8_t flags = expression != NULL ? __luauc_function_flags(expression, 0) : main_flags;

    compiler->current_function = function;
    compiler->current_body = body;
    compiler->register_top = 0;
    compiler->stack_size = 0;
    compiler->argument_count = argument_count + (has_self ? 1 : 0);
    compiler->locals.size = 0;
    compiler->loops.size = 0;
    compiler->loop_jumps.size = 0;
    function->upvalues.size = 0;

    id = luauc_bytecode_begin_function(
        &compiler->bytecode, (uint8_t)compiler->argument_count, vararg
    );
    if (id == UINT32_MAX)
        __luauc_compiler_raise(compiler, body->location, "Out of memory");
    function->id = id;
    __luauc_set_function_type_info(compiler, expression);

    __luauc_set_line(compiler, expression != NULL ? expression->location : body->location);
    if (vararg)
        luauc_bytecode_emit_abc(
            &compiler->bytecode,
            LOP_PREPVARARGS,
            (uint8_t)compiler->argument_count,
            0,
            0
        );
    argument_registers = __luauc_allocate_registers(
        compiler, (unsigned int)compiler->argument_count, body->location
    );
    if (has_self)
        __luauc_push_local(
            compiler,
            expression->value.function.self,
            argument_registers,
            luauc_bytecode_get_debug_pc(&compiler->bytecode)
        );
    for (index = 0; index < argument_count; ++index)
        __luauc_push_local(
            compiler,
            arguments[index],
            (uint8_t)(argument_registers + (has_self ? 1 : 0) + index),
            luauc_bytecode_get_debug_pc(&compiler->bytecode)
        );

    __luauc_compile_block(compiler, body, 0);
    if (!__luauc_statement_terminates(body))
    {
        __luauc_set_line(compiler, body->location);
        __luauc_emit_close_upvalues(compiler, 0);
        luauc_bytecode_emit_abc(&compiler->bytecode, LOP_RETURN, 0, 1, 0);
    }

    luauc_bytecode_set_debug_function_line_defined(
        &compiler->bytecode,
        expression != NULL ? (int)expression->location.begin.line + 1 :
            (int)body->location.begin.line + 1
    );
    if (compiler->options.debugLevel >= 1 && expression != NULL &&
        expression->value.function.debugname.value != NULL)
        luauc_bytecode_set_debug_function_name(
            &compiler->bytecode, __luauc_name_ref(expression->value.function.debugname)
        );
    if (compiler->options.debugLevel >= 2)
    {
        uint32_t endpc = luauc_bytecode_get_debug_pc(&compiler->bytecode);
        for (index = 0; index < compiler->locals.size; ++index)
        {
            luauc_compiler_local_t* local =
                (luauc_compiler_local_t*)luauc_vector_at(&compiler->locals, index);
            luauc_bytecode_push_debug_local(
                &compiler->bytecode,
                __luauc_name_ref(local->local->name),
                local->reg,
                local->startpc,
                endpc
            );
        }
        for (index = 0; index < function->upvalues.size; ++index)
        {
            luauc_ast_local_t* local =
                *(luauc_ast_local_t**)luauc_vector_at(&function->upvalues, index);
            luauc_bytecode_push_debug_upval(
                &compiler->bytecode, __luauc_name_ref(local->name)
            );
        }
    }
    if (compiler->options.typeInfoLevel >= 1)
    {
        uint32_t endpc = luauc_bytecode_get_debug_pc(&compiler->bytecode);
        for (index = compiler->argument_count; index < compiler->locals.size; ++index)
        {
            luauc_compiler_local_t* local =
                (luauc_compiler_local_t*)luauc_vector_at(&compiler->locals, index);
            luauc_bytecode_push_local_type_info(
                &compiler->bytecode,
                __luauc_compiler_type(compiler, local->local->annotation, expression),
                local->reg,
                local->startpc,
                endpc
            );
        }
        for (index = 0; index < function->upvalues.size; ++index)
        {
            luauc_ast_local_t* local =
                *(luauc_ast_local_t**)luauc_vector_at(&function->upvalues, index);
            luauc_bytecode_push_upval_type_info(
                &compiler->bytecode,
                __luauc_compiler_type(compiler, local->annotation, expression)
            );
        }
    }

    if (compiler->options.optimizationLevel >= 1)
        luauc_bytecode_fold_jumps(&compiler->bytecode);
    if (!luauc_bytecode_expand_jumps(&compiler->bytecode))
        __luauc_compiler_raise(
            compiler, body->location, "Exceeded jump distance limit; simplify the code to compile"
        );
    if (!luauc_bytecode_end_function(
            &compiler->bytecode,
            (uint8_t)compiler->stack_size,
            (uint8_t)function->upvalues.size,
            flags,
            0
        ))
        __luauc_compiler_raise(compiler, body->location, "Out of memory");
    compiler->current_function = NULL;
    compiler->current_body = NULL;
    return id;
}

static void __luauc_compiler_destroy(luauc_compiler_t* compiler)
{
    size_t index;
    for (index = 0; index < compiler->functions.size; ++index)
    {
        luauc_compiler_function_t* function =
            (luauc_compiler_function_t*)luauc_vector_at(&compiler->functions, index);
        luauc_vector_destroy(&function->upvalues);
    }
    for (index = 0; index < compiler->owned_strings.size; ++index)
    {
        luauc_owned_string_t* string =
            (luauc_owned_string_t*)luauc_vector_at(&compiler->owned_strings, index);
        compiler->allocator.reallocate(
            compiler->allocator.context, string->data, string->size == 0 ? 1 : string->size, 0
        );
    }
    luauc_vector_destroy(&compiler->functions);
    luauc_vector_destroy(&compiler->locals);
    luauc_vector_destroy(&compiler->loops);
    luauc_vector_destroy(&compiler->loop_jumps);
    luauc_vector_destroy(&compiler->owned_strings);
    luauc_vector_destroy(&compiler->mutable_globals);
    luauc_vector_destroy(&compiler->userdata_types);
    luauc_bytecode_builder_destroy(&compiler->bytecode);
    if (compiler->error_message != NULL)
        compiler->allocator.reallocate(
            compiler->allocator.context,
            compiler->error_message,
            compiler->error_capacity,
            0
        );
}

static int __luauc_compiler_init(
    luauc_compiler_t* compiler, const lua_CompileOptions* input, luauc_allocator_t allocator
)
{
    memset(compiler, 0, sizeof(*compiler));
    compiler->allocator = allocator.reallocate != NULL ? allocator : luauc_default_allocator();
    compiler->options.optimizationLevel = 1;
    compiler->options.debugLevel = 1;
    compiler->options.typeInfoLevel = 0;
    compiler->options.coverageLevel = 0;
    if (input != NULL)
        compiler->options = *input;
    if (compiler->options.optimizationLevel < 0)
        compiler->options.optimizationLevel = 0;
    if (compiler->options.optimizationLevel > 2)
        compiler->options.optimizationLevel = 2;
    if (compiler->options.debugLevel < 0)
        compiler->options.debugLevel = 0;
    if (compiler->options.debugLevel > 2)
        compiler->options.debugLevel = 2;

    if (!luauc_vector_init(
            &compiler->functions, sizeof(luauc_compiler_function_t), compiler->allocator
        ) ||
        !luauc_vector_init(
            &compiler->locals, sizeof(luauc_compiler_local_t), compiler->allocator
        ) ||
        !luauc_vector_init(
            &compiler->loops, sizeof(luauc_compiler_loop_t), compiler->allocator
        ) ||
        !luauc_vector_init(
            &compiler->loop_jumps, sizeof(luauc_loop_jump_t), compiler->allocator
        ) ||
        !luauc_vector_init(
            &compiler->owned_strings, sizeof(luauc_owned_string_t), compiler->allocator
        ) ||
        !luauc_vector_init(
            &compiler->mutable_globals, sizeof(luauc_string_ref_t), compiler->allocator
        ) ||
        !luauc_vector_init(
            &compiler->userdata_types,
            sizeof(luauc_compiler_userdata_type_t),
            compiler->allocator
        ) ||
        !luauc_bytecode_builder_init(&compiler->bytecode, compiler->allocator))
    {
        __luauc_compiler_destroy(compiler);
        return 0;
    }
    return 1;
}

static int __luauc_comment_equals(const luauc_hot_comment_t* comment, const char* text)
{
    size_t length = strlen(text);
    return comment->text.length == length &&
        memcmp(comment->text.data, text, length) == 0;
}

static uint8_t __luauc_apply_hot_comments(
    luauc_compiler_t* compiler, const luauc_array_t* comments
)
{
    size_t index;
    uint8_t flags = LPF_NATIVE_COLD;
    if (comments == NULL)
        return flags;
    for (index = 0; index < comments->size; ++index)
    {
        const luauc_hot_comment_t* comment =
            &((const luauc_hot_comment_t*)comments->data)[index];
        if (!comment->header)
            continue;
        if (comment->text.length > 9 &&
            memcmp(comment->text.data, "optimize ", 9) == 0)
        {
            int level = comment->text.data[9] - '0';
            if (level >= 0 && level <= 2)
                compiler->options.optimizationLevel = level;
        }
        else if (__luauc_comment_equals(comment, "native"))
        {
            flags = (uint8_t)(flags | LPF_NATIVE_MODULE);
            compiler->options.optimizationLevel = 2;
            compiler->options.typeInfoLevel = 1;
        }
    }
    return flags;
}

int luauc_compile_tree(
    luauc_ast_stat_t* root,
    const luauc_array_t* hot_comments,
    const luauc_name_table_t* names,
    const lua_CompileOptions* options,
    luauc_allocator_t allocator,
    luauc_compile_result_t* result
)
{
    luauc_compiler_t compiler;
    luauc_compiler_function_t main_function;
    uint8_t main_flags;
    size_t index;
    int jumped;

    if (result == NULL || root == NULL || names == NULL)
        return 0;
    memset(result, 0, sizeof(*result));
    if (!__luauc_compiler_init(&compiler, options, allocator))
        return 0;
    memset(&main_function, 0, sizeof(main_function));

    jumped = setjmp(compiler.error_jump);
    if (jumped == 0)
    {
        main_flags = __luauc_apply_hot_comments(&compiler, hot_comments);
        __luauc_initialize_userdata_types(&compiler, names, root->location);
        __luauc_collect_statement_functions(&compiler, root);
        for (index = 0; index < compiler.functions.size; ++index)
        {
            luauc_compiler_function_t* function =
                (luauc_compiler_function_t*)luauc_vector_at(&compiler.functions, index);
            __luauc_compile_function(&compiler, function, NULL, 0);
            if ((__luauc_function_flags(function->expression, 0) & LPF_NATIVE_FUNCTION) != 0 &&
                (main_flags & LPF_NATIVE_MODULE) == 0)
                main_flags = (uint8_t)(main_flags | LPF_NATIVE_FUNCTION);
        }
        __luauc_compiler_check(
            &compiler,
            luauc_vector_init(
                &main_function.upvalues, sizeof(luauc_ast_local_t*), compiler.allocator
            ),
            root->location
        );
        luauc_bytecode_set_main_function(
            &compiler.bytecode,
            __luauc_compile_function(&compiler, &main_function, root, main_flags)
        );
        if (main_function.upvalues.size != 0)
            __luauc_compiler_raise(&compiler, root->location, "Main function has upvalues");
        luauc_vector_destroy(&main_function.upvalues);
        if (!luauc_bytecode_finalize(&compiler.bytecode))
            __luauc_compiler_raise(&compiler, root->location, "Out of memory");
        result->bytecode = luauc_bytecode_release(
            &compiler.bytecode, &result->bytecode_size
        );
        if (result->bytecode == NULL)
            __luauc_compiler_raise(&compiler, root->location, "Out of memory");
        __luauc_compiler_destroy(&compiler);
        return 1;
    }

    if (main_function.upvalues.element_size != 0)
        luauc_vector_destroy(&main_function.upvalues);
    result->has_error = 1;
    result->error_location = compiler.error_location;
    if (compiler.error_message != NULL)
    {
        result->error_message = compiler.error_message;
        compiler.error_message = NULL;
        compiler.error_capacity = 0;
    }
    else
    {
        static const char __message[] = "Out of memory";
        result->error_message = (char*)compiler.allocator.reallocate(
            compiler.allocator.context, NULL, 0, sizeof(__message)
        );
        if (result->error_message != NULL)
            memcpy(result->error_message, __message, sizeof(__message));
    }
    __luauc_compiler_destroy(&compiler);
    return result->error_message != NULL;
}

void luauc_compile_result_destroy(luauc_compile_result_t* result, luauc_allocator_t allocator)
{
    luauc_allocator_t actual = allocator.reallocate != NULL ? allocator : luauc_default_allocator();
    if (result == NULL)
        return;
    if (result->bytecode != NULL)
        actual.reallocate(
            actual.context, result->bytecode, result->bytecode_size, 0
        );
    if (result->error_message != NULL)
        actual.reallocate(
            actual.context, result->error_message, strlen(result->error_message) + 1, 0
        );
    memset(result, 0, sizeof(*result));
}
