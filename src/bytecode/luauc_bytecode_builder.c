// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "bytecode/luauc_bytecode_builder.h"

#include "luauc_bytecode_utils.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum
{
    LUAUC_MAX_CONSTANT_COUNT = 1 << 23,
    LUAUC_MAX_CLOSURE_COUNT = 1 << 15,
    LUAUC_MAX_JUMP_DISTANCE = 1 << 23
};

typedef enum luauc_constant_type_t
{
    LUAUC_CONSTANT_NIL,
    LUAUC_CONSTANT_BOOLEAN,
    LUAUC_CONSTANT_NUMBER,
    LUAUC_CONSTANT_INTEGER,
    LUAUC_CONSTANT_VECTOR,
    LUAUC_CONSTANT_STRING,
    LUAUC_CONSTANT_IMPORT,
    LUAUC_CONSTANT_TABLE,
    LUAUC_CONSTANT_CLOSURE,
    LUAUC_CONSTANT_CLASS_SHAPE
} luauc_constant_type_t;

typedef struct luauc_constant_t
{
    luauc_constant_type_t type;
    union
    {
        int boolean_value;
        double number_value;
        int64_t integer_value;
        float vector_value[4];
        uint32_t string_value;
        uint32_t import_value;
        uint32_t table_value;
        uint32_t closure_value;
        uint32_t class_shape_value;
    } value;
} luauc_constant_t;

typedef struct luauc_constant_key_t
{
    luauc_constant_type_t type;
    uint64_t value;
    uint64_t extra;
} luauc_constant_key_t;

typedef struct luauc_bytecode_function_t
{
    luauc_buffer_t data;
    luauc_buffer_t typeinfo;
    uint8_t maxstacksize;
    uint8_t numparams;
    uint8_t numupvalues;
    uint8_t isvararg;
    uint32_t debugname;
    int debuglinedefined;
} luauc_bytecode_function_t;

typedef struct luauc_jump_t
{
    uint32_t source;
    uint32_t target;
} luauc_jump_t;

typedef struct luauc_debug_local_t
{
    uint32_t name;
    uint8_t reg;
    uint32_t startpc;
    uint32_t endpc;
} luauc_debug_local_t;

typedef struct luauc_debug_upval_t
{
    uint32_t name;
} luauc_debug_upval_t;

typedef struct luauc_typed_local_t
{
    uint8_t type;
    uint8_t reg;
    uint32_t startpc;
    uint32_t endpc;
} luauc_typed_local_t;

typedef struct luauc_typed_upval_t
{
    uint8_t type;
} luauc_typed_upval_t;

typedef struct luauc_userdata_type_t
{
    char* name;
    size_t length;
    uint32_t name_ref;
    int used;
} luauc_userdata_type_t;

typedef struct luauc_stored_class_shape_t
{
    int32_t class_name;
    luauc_vector_t property_names;
    luauc_vector_t method_names;
} luauc_stored_class_shape_t;

_Static_assert(sizeof(float) == 4, "Luau bytecode requires 32-bit float");
_Static_assert(sizeof(double) == 8, "Luau bytecode requires 64-bit double");
_Static_assert(LBC_VERSION_TARGET >= LBC_VERSION_MIN && LBC_VERSION_TARGET <= LBC_VERSION_MAX, "Invalid bytecode version setup");
_Static_assert(LBC_VERSION_MAX <= 127, "Bytecode version must fit in a 7-bit varint");

static int __luauc_builder_fail(luauc_bytecode_builder_t* builder)
{
    builder->failed = 1;
    return 0;
}

static void* __luauc_builder_push(luauc_bytecode_builder_t* builder, luauc_vector_t* vector, const void* value)
{
    void* result = luauc_vector_push(vector, value);
    if (result == NULL)
        builder->failed = 1;
    return result;
}

static int __luauc_buffer_append_u64_varint(luauc_buffer_t* buffer, uint64_t value)
{
    do
    {
        uint8_t byte = (uint8_t)(value & UINT64_C(127));
        value >>= 7;
        if (value != 0)
            byte = (uint8_t)(byte | 0x80u);
        if (!luauc_buffer_append_byte(buffer, byte))
            return 0;
    } while (value != 0);
    return 1;
}

static int __luauc_buffer_append_i32(luauc_buffer_t* buffer, int32_t value)
{
    return luauc_buffer_append_u32(buffer, (uint32_t)value);
}

static int __luauc_buffer_append_float(luauc_buffer_t* buffer, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return luauc_buffer_append_u32(buffer, bits);
}

static int __luauc_buffer_append_double(luauc_buffer_t* buffer, double value)
{
    uint64_t bits;
    unsigned char bytes[8];
    size_t index;
    memcpy(&bits, &value, sizeof(bits));
    for (index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (unsigned char)(bits >> (index * 8));
    return luauc_buffer_append(buffer, bytes, sizeof(bytes));
}

static int __luauc_string_ref_equal(luauc_string_ref_t left, luauc_string_ref_t right)
{
    if (left.data == NULL || right.data == NULL)
        return left.data == right.data;
    return left.length == right.length && memcmp(left.data, right.data, left.length) == 0;
}

static int __luauc_table_shape_equal(const luauc_table_shape_t* left, const luauc_table_shape_t* right)
{
    if (left->length != right->length || left->has_constants != right->has_constants)
        return 0;
    if (memcmp(left->keys, right->keys, left->length * sizeof(left->keys[0])) != 0)
        return 0;
    return !left->has_constants ||
        memcmp(left->constants, right->constants, left->length * sizeof(left->constants[0])) == 0;
}

static uint32_t __luauc_add_string(luauc_bytecode_builder_t* builder, luauc_string_ref_t value)
{
    size_t index;

    if (value.data == NULL)
        return 0;

    for (index = 0; index < builder->strings.size; ++index)
    {
        const luauc_string_ref_t* existing = (const luauc_string_ref_t*)luauc_vector_at_const(&builder->strings, index);
        if (__luauc_string_ref_equal(*existing, value))
            return (uint32_t)index + 1;
    }

    if (builder->strings.size >= UINT32_MAX || __luauc_builder_push(builder, &builder->strings, &value) == NULL)
        return 0;

    return (uint32_t)builder->strings.size;
}

static int32_t __luauc_add_constant(
    luauc_bytecode_builder_t* builder, const luauc_constant_key_t* key, const luauc_constant_t* constant
)
{
    size_t index;

    for (index = 0; index < builder->constants.size; ++index)
    {
        const luauc_constant_t* current = (const luauc_constant_t*)luauc_vector_at_const(&builder->constants, index);
        luauc_constant_key_t current_key;
        memset(&current_key, 0, sizeof(current_key));
        current_key.type = current->type;

        switch (current->type)
        {
        case LUAUC_CONSTANT_NIL:
            break;
        case LUAUC_CONSTANT_BOOLEAN:
            current_key.value = (uint64_t)current->value.boolean_value;
            break;
        case LUAUC_CONSTANT_NUMBER:
            memcpy(&current_key.value, &current->value.number_value, sizeof(current_key.value));
            break;
        case LUAUC_CONSTANT_INTEGER:
            memcpy(&current_key.value, &current->value.integer_value, sizeof(current_key.value));
            break;
        case LUAUC_CONSTANT_VECTOR:
            memcpy(&current_key.value, &current->value.vector_value[0], sizeof(current_key.value));
            memcpy(&current_key.extra, &current->value.vector_value[2], sizeof(current_key.extra));
            break;
        case LUAUC_CONSTANT_STRING:
            current_key.value = current->value.string_value;
            break;
        case LUAUC_CONSTANT_IMPORT:
            current_key.value = current->value.import_value;
            break;
        case LUAUC_CONSTANT_CLOSURE:
            current_key.value = current->value.closure_value;
            break;
        case LUAUC_CONSTANT_TABLE:
        case LUAUC_CONSTANT_CLASS_SHAPE:
            continue;
        }

        if (current_key.type == key->type && current_key.value == key->value && current_key.extra == key->extra)
            return (int32_t)index;
    }

    if (builder->constants.size >= LUAUC_MAX_CONSTANT_COUNT ||
        __luauc_builder_push(builder, &builder->constants, constant) == NULL)
        return -1;
    return (int32_t)(builder->constants.size - 1);
}

static void __luauc_clear_function_state(luauc_bytecode_builder_t* builder)
{
    luauc_vector_clear(&builder->instructions);
    luauc_vector_clear(&builder->lines);
    luauc_vector_clear(&builder->constants);
    luauc_vector_clear(&builder->protos);
    luauc_vector_clear(&builder->jumps);
    luauc_vector_clear(&builder->feedback_slots);
    luauc_vector_clear(&builder->table_shapes);
    luauc_vector_clear(&builder->debug_locals);
    luauc_vector_clear(&builder->debug_upvals);
    luauc_vector_clear(&builder->typed_locals);
    luauc_vector_clear(&builder->typed_upvals);
}

static int __luauc_init_vectors(luauc_bytecode_builder_t* builder)
{
#define LUAUC_INIT_VECTOR(field, type) \
    do \
    { \
        if (!luauc_vector_init(&builder->field, sizeof(type), builder->allocator)) \
            return 0; \
    } while (0)
    LUAUC_INIT_VECTOR(functions, luauc_bytecode_function_t);
    LUAUC_INIT_VECTOR(instructions, uint32_t);
    LUAUC_INIT_VECTOR(lines, int);
    LUAUC_INIT_VECTOR(constants, luauc_constant_t);
    LUAUC_INIT_VECTOR(protos, uint32_t);
    LUAUC_INIT_VECTOR(jumps, luauc_jump_t);
    LUAUC_INIT_VECTOR(table_shapes, luauc_table_shape_t);
    LUAUC_INIT_VECTOR(class_shapes, luauc_stored_class_shape_t);
    LUAUC_INIT_VECTOR(feedback_slots, uint32_t);
    LUAUC_INIT_VECTOR(debug_locals, luauc_debug_local_t);
    LUAUC_INIT_VECTOR(debug_upvals, luauc_debug_upval_t);
    LUAUC_INIT_VECTOR(typed_locals, luauc_typed_local_t);
    LUAUC_INIT_VECTOR(typed_upvals, luauc_typed_upval_t);
    LUAUC_INIT_VECTOR(userdata_types, luauc_userdata_type_t);
    LUAUC_INIT_VECTOR(strings, luauc_string_ref_t);
#undef LUAUC_INIT_VECTOR
    return 1;
}

int luauc_bytecode_builder_init(luauc_bytecode_builder_t* builder, luauc_allocator_t allocator)
{
    if (builder == NULL)
        return 0;
    memset(builder, 0, sizeof(*builder));
    builder->allocator = allocator.reallocate != NULL ? allocator : luauc_default_allocator();
    builder->current_function = UINT32_MAX;
    builder->main_function = UINT32_MAX;
    luauc_buffer_init(&builder->bytecode, builder->allocator);

    if (!__luauc_init_vectors(builder))
    {
        luauc_bytecode_builder_destroy(builder);
        return 0;
    }

    if (!luauc_vector_reserve(&builder->instructions, 32) || !luauc_vector_reserve(&builder->lines, 32) ||
        !luauc_vector_reserve(&builder->constants, 16) || !luauc_vector_reserve(&builder->protos, 16) ||
        !luauc_vector_reserve(&builder->functions, 8))
    {
        luauc_bytecode_builder_destroy(builder);
        return 0;
    }
    return 1;
}

void luauc_bytecode_builder_destroy(luauc_bytecode_builder_t* builder)
{
    size_t index;
    if (builder == NULL)
        return;

    for (index = 0; index < builder->functions.size; ++index)
    {
        luauc_bytecode_function_t* function = (luauc_bytecode_function_t*)luauc_vector_at(&builder->functions, index);
        luauc_buffer_destroy(&function->data);
        luauc_buffer_destroy(&function->typeinfo);
    }
    for (index = 0; index < builder->class_shapes.size; ++index)
    {
        luauc_stored_class_shape_t* shape = (luauc_stored_class_shape_t*)luauc_vector_at(&builder->class_shapes, index);
        luauc_vector_destroy(&shape->property_names);
        luauc_vector_destroy(&shape->method_names);
    }
    for (index = 0; index < builder->userdata_types.size; ++index)
    {
        luauc_userdata_type_t* type = (luauc_userdata_type_t*)luauc_vector_at(&builder->userdata_types, index);
        if (type->name != NULL)
            builder->allocator.reallocate(builder->allocator.context, type->name, type->length + 1, 0);
    }

    luauc_vector_destroy(&builder->functions);
    luauc_vector_destroy(&builder->instructions);
    luauc_vector_destroy(&builder->lines);
    luauc_vector_destroy(&builder->constants);
    luauc_vector_destroy(&builder->protos);
    luauc_vector_destroy(&builder->jumps);
    luauc_vector_destroy(&builder->table_shapes);
    luauc_vector_destroy(&builder->class_shapes);
    luauc_vector_destroy(&builder->feedback_slots);
    luauc_vector_destroy(&builder->debug_locals);
    luauc_vector_destroy(&builder->debug_upvals);
    luauc_vector_destroy(&builder->typed_locals);
    luauc_vector_destroy(&builder->typed_upvals);
    luauc_vector_destroy(&builder->userdata_types);
    luauc_vector_destroy(&builder->strings);
    luauc_buffer_destroy(&builder->bytecode);
    memset(builder, 0, sizeof(*builder));
}

uint32_t luauc_bytecode_begin_function(luauc_bytecode_builder_t* builder, uint8_t numparams, int isvararg)
{
    luauc_bytecode_function_t function;
    uint32_t id;

    assert(builder != NULL && builder->current_function == UINT32_MAX);
    if (builder == NULL || builder->current_function != UINT32_MAX || builder->functions.size >= UINT32_MAX)
        return UINT32_MAX;

    memset(&function, 0, sizeof(function));
    luauc_buffer_init(&function.data, builder->allocator);
    luauc_buffer_init(&function.typeinfo, builder->allocator);
    function.numparams = numparams;
    function.isvararg = (uint8_t)(isvararg != 0);

    id = (uint32_t)builder->functions.size;
    if (__luauc_builder_push(builder, &builder->functions, &function) == NULL)
    {
        luauc_buffer_destroy(&function.data);
        luauc_buffer_destroy(&function.typeinfo);
        return UINT32_MAX;
    }

    builder->current_function = id;
    builder->has_long_jumps = 0;
    builder->debug_line = 0;
    return id;
}

void luauc_bytecode_set_main_function(luauc_bytecode_builder_t* builder, uint32_t function_id)
{
    assert(builder != NULL && function_id < builder->functions.size);
    if (builder != NULL && function_id < builder->functions.size)
        builder->main_function = function_id;
}

static int __luauc_append_current_instruction(luauc_bytecode_builder_t* builder, uint32_t instruction)
{
    int line = builder->debug_line;
    if (__luauc_builder_push(builder, &builder->instructions, &instruction) == NULL)
        return 0;
    if (__luauc_builder_push(builder, &builder->lines, &line) == NULL)
    {
        builder->instructions.size--;
        return 0;
    }
    return 1;
}

void luauc_bytecode_emit_abc(luauc_bytecode_builder_t* builder, luauc_opcode_t op, uint8_t a, uint8_t b, uint8_t c)
{
    uint32_t instruction = (uint32_t)op | ((uint32_t)a << 8) | ((uint32_t)b << 16) | ((uint32_t)c << 24);
    if (builder != NULL)
        __luauc_append_current_instruction(builder, instruction);
}

void luauc_bytecode_emit_ad(luauc_bytecode_builder_t* builder, luauc_opcode_t op, uint8_t a, int16_t d)
{
    uint32_t instruction = (uint32_t)op | ((uint32_t)a << 8) | ((uint32_t)(uint16_t)d << 16);
    if (builder != NULL)
        __luauc_append_current_instruction(builder, instruction);
}

void luauc_bytecode_emit_e(luauc_bytecode_builder_t* builder, luauc_opcode_t op, int32_t e)
{
    uint32_t instruction = (uint32_t)op | ((uint32_t)e << 8);
    if (builder != NULL)
        __luauc_append_current_instruction(builder, instruction);
}

void luauc_bytecode_emit_aux(luauc_bytecode_builder_t* builder, uint32_t aux)
{
    if (builder != NULL)
        __luauc_append_current_instruction(builder, aux);
}

void luauc_bytecode_undo_emit(luauc_bytecode_builder_t* builder, luauc_opcode_t op)
{
    uint32_t* last;
    assert(builder != NULL && builder->instructions.size != 0);
    if (builder == NULL || builder->instructions.size == 0)
        return;
    last = (uint32_t*)luauc_vector_at(&builder->instructions, builder->instructions.size - 1);
    assert((*last & 0xffu) == (uint32_t)op);
    (void)last;
    (void)op;
    builder->instructions.size--;
    builder->lines.size--;
}

size_t luauc_bytecode_emit_label(const luauc_bytecode_builder_t* builder)
{
    return builder == NULL ? 0 : builder->instructions.size;
}

int luauc_bytecode_patch_jump_d(luauc_bytecode_builder_t* builder, size_t jump_label, size_t target_label)
{
    uint32_t* instruction;
    int64_t offset;
    luauc_jump_t jump;

    if (builder == NULL || jump_label >= builder->instructions.size || target_label > builder->instructions.size)
        return 0;
    instruction = (uint32_t*)luauc_vector_at(&builder->instructions, jump_label);
    assert(__luauc_is_jump_d((luauc_opcode_t)LUAU_INSN_OP(*instruction)));
    assert(LUAU_INSN_D(*instruction) == 0);
    offset = (int64_t)target_label - (int64_t)jump_label - 1;

    if (offset >= INT16_MIN && offset <= INT16_MAX)
        *instruction |= (uint32_t)(uint16_t)(int16_t)offset << 16;
    else if (offset > -LUAUC_MAX_JUMP_DISTANCE && offset < LUAUC_MAX_JUMP_DISTANCE)
        builder->has_long_jumps = 1;
    else
        return 0;

    jump.source = (uint32_t)jump_label;
    jump.target = (uint32_t)target_label;
    return __luauc_builder_push(builder, &builder->jumps, &jump) != NULL;
}

int luauc_bytecode_patch_skip_c(luauc_bytecode_builder_t* builder, size_t jump_label, size_t target_label)
{
    uint32_t* instruction;
    size_t offset;
    luauc_opcode_t op;

    if (builder == NULL || jump_label >= builder->instructions.size || target_label <= jump_label)
        return 0;
    instruction = (uint32_t*)luauc_vector_at(&builder->instructions, jump_label);
    op = (luauc_opcode_t)LUAU_INSN_OP(*instruction);
    assert(__luauc_is_skip_c(op) || __luauc_is_fast_call(op));
    (void)op;
    assert(LUAU_INSN_C(*instruction) == 0);
    offset = target_label - jump_label - 1;
    if (offset > UINT8_MAX)
        return 0;
    *instruction |= (uint32_t)offset << 24;
    return 1;
}

void luauc_bytecode_patch_aux(luauc_bytecode_builder_t* builder, size_t target_aux, int32_t new_value)
{
    uint32_t* instruction;
    assert(builder != NULL && target_aux < builder->instructions.size);
    if (builder == NULL || target_aux >= builder->instructions.size)
        return;
    instruction = (uint32_t*)luauc_vector_at(&builder->instructions, target_aux);
    *instruction = (uint32_t)new_value;
}

static luauc_constant_key_t __luauc_make_constant_key(luauc_constant_type_t type)
{
    luauc_constant_key_t key;
    memset(&key, 0, sizeof(key));
    key.type = type;
    return key;
}

int32_t luauc_bytecode_add_constant_nil(luauc_bytecode_builder_t* builder)
{
    luauc_constant_t constant;
    luauc_constant_key_t key = __luauc_make_constant_key(LUAUC_CONSTANT_NIL);
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_NIL;
    return __luauc_add_constant(builder, &key, &constant);
}

int32_t luauc_bytecode_add_constant_boolean(luauc_bytecode_builder_t* builder, int value)
{
    luauc_constant_t constant;
    luauc_constant_key_t key = __luauc_make_constant_key(LUAUC_CONSTANT_BOOLEAN);
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_BOOLEAN;
    constant.value.boolean_value = value != 0;
    key.value = (uint64_t)constant.value.boolean_value;
    return __luauc_add_constant(builder, &key, &constant);
}

int32_t luauc_bytecode_add_constant_number(luauc_bytecode_builder_t* builder, double value)
{
    luauc_constant_t constant;
    luauc_constant_key_t key = __luauc_make_constant_key(LUAUC_CONSTANT_NUMBER);
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_NUMBER;
    constant.value.number_value = value;
    memcpy(&key.value, &value, sizeof(value));
    return __luauc_add_constant(builder, &key, &constant);
}

int32_t luauc_bytecode_add_constant_integer(luauc_bytecode_builder_t* builder, int64_t value)
{
    luauc_constant_t constant;
    luauc_constant_key_t key = __luauc_make_constant_key(LUAUC_CONSTANT_INTEGER);
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_INTEGER;
    constant.value.integer_value = value;
    memcpy(&key.value, &value, sizeof(value));
    return __luauc_add_constant(builder, &key, &constant);
}

int32_t luauc_bytecode_add_constant_vector(luauc_bytecode_builder_t* builder, float x, float y, float z, float w)
{
    luauc_constant_t constant;
    luauc_constant_key_t key = __luauc_make_constant_key(LUAUC_CONSTANT_VECTOR);
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_VECTOR;
    constant.value.vector_value[0] = x;
    constant.value.vector_value[1] = y;
    constant.value.vector_value[2] = z;
    constant.value.vector_value[3] = w;
    memcpy(&key.value, &constant.value.vector_value[0], sizeof(key.value));
    memcpy(&key.extra, &constant.value.vector_value[2], sizeof(key.extra));
    return __luauc_add_constant(builder, &key, &constant);
}

int32_t luauc_bytecode_add_constant_string(luauc_bytecode_builder_t* builder, luauc_string_ref_t value)
{
    uint32_t string_index;
    luauc_constant_t constant;
    luauc_constant_key_t key = __luauc_make_constant_key(LUAUC_CONSTANT_STRING);
    string_index = __luauc_add_string(builder, value);
    if (string_index == 0 && value.data != NULL)
        return -1;
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_STRING;
    constant.value.string_value = string_index;
    key.value = string_index;
    return __luauc_add_constant(builder, &key, &constant);
}

int32_t luauc_bytecode_add_import(luauc_bytecode_builder_t* builder, uint32_t import_id)
{
    luauc_constant_t constant;
    luauc_constant_key_t key = __luauc_make_constant_key(LUAUC_CONSTANT_IMPORT);
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_IMPORT;
    constant.value.import_value = import_id;
    key.value = import_id;
    return __luauc_add_constant(builder, &key, &constant);
}

int32_t luauc_bytecode_add_constant_table(luauc_bytecode_builder_t* builder, const luauc_table_shape_t* shape)
{
    size_t index;
    luauc_constant_t constant;

    if (builder == NULL || shape == NULL || shape->length > LUAUC_TABLE_SHAPE_MAX_LENGTH)
        return -1;
    for (index = 0; index < builder->table_shapes.size; ++index)
    {
        const luauc_table_shape_t* existing = (const luauc_table_shape_t*)luauc_vector_at_const(&builder->table_shapes, index);
        if (__luauc_table_shape_equal(existing, shape))
        {
            size_t constant_index;
            for (constant_index = 0; constant_index < builder->constants.size; ++constant_index)
            {
                const luauc_constant_t* current =
                    (const luauc_constant_t*)luauc_vector_at_const(&builder->constants, constant_index);
                if (current->type == LUAUC_CONSTANT_TABLE && current->value.table_value == index)
                    return (int32_t)constant_index;
            }
        }
    }

    if (builder->constants.size >= LUAUC_MAX_CONSTANT_COUNT)
        return -1;
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_TABLE;
    constant.value.table_value = (uint32_t)builder->table_shapes.size;
    if (__luauc_builder_push(builder, &builder->table_shapes, shape) == NULL ||
        __luauc_builder_push(builder, &builder->constants, &constant) == NULL)
        return -1;
    return (int32_t)(builder->constants.size - 1);
}

int32_t luauc_bytecode_add_constant_closure(luauc_bytecode_builder_t* builder, uint32_t function_id)
{
    luauc_constant_t constant;
    luauc_constant_key_t key = __luauc_make_constant_key(LUAUC_CONSTANT_CLOSURE);
    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_CLOSURE;
    constant.value.closure_value = function_id;
    key.value = function_id;
    return __luauc_add_constant(builder, &key, &constant);
}

int32_t luauc_bytecode_add_class_shape(luauc_bytecode_builder_t* builder, const luauc_class_shape_t* shape)
{
    luauc_stored_class_shape_t stored;
    luauc_constant_t constant;
    size_t index;

    if (builder == NULL || shape == NULL || builder->constants.size >= LUAUC_MAX_CONSTANT_COUNT)
        return -1;
    memset(&stored, 0, sizeof(stored));
    stored.class_name = shape->class_name;
    luauc_vector_init(&stored.property_names, sizeof(int32_t), builder->allocator);
    luauc_vector_init(&stored.method_names, sizeof(int32_t), builder->allocator);
    for (index = 0; index < shape->property_count; ++index)
        if (luauc_vector_push(&stored.property_names, &shape->property_names[index]) == NULL)
            goto fail;
    for (index = 0; index < shape->method_count; ++index)
        if (luauc_vector_push(&stored.method_names, &shape->method_names[index]) == NULL)
            goto fail;

    memset(&constant, 0, sizeof(constant));
    constant.type = LUAUC_CONSTANT_CLASS_SHAPE;
    constant.value.class_shape_value = (uint32_t)builder->class_shapes.size;
    if (__luauc_builder_push(builder, &builder->class_shapes, &stored) == NULL)
        goto fail;
    if (__luauc_builder_push(builder, &builder->constants, &constant) == NULL)
        return -1;
    return (int32_t)(builder->constants.size - 1);

fail:
    luauc_vector_destroy(&stored.property_names);
    luauc_vector_destroy(&stored.method_names);
    return __luauc_builder_fail(builder), -1;
}

uint32_t luauc_bytecode_add_feedback_slot(luauc_bytecode_builder_t* builder, luauc_feedback_type_t type)
{
    uint32_t pc;
    assert(type == LFT_CALLTARGET);
    if (builder == NULL || type != LFT_CALLTARGET || builder->feedback_slots.size >= UINT32_MAX)
        return UINT32_MAX;
    pc = (uint32_t)builder->instructions.size;
    if (__luauc_builder_push(builder, &builder->feedback_slots, &pc) == NULL)
        return UINT32_MAX;
    return (uint32_t)(builder->feedback_slots.size - 1);
}

int16_t luauc_bytecode_add_child_function(luauc_bytecode_builder_t* builder, uint32_t function_id)
{
    size_t index;
    if (builder == NULL)
        return -1;
    for (index = 0; index < builder->protos.size; ++index)
        if (*(const uint32_t*)luauc_vector_at_const(&builder->protos, index) == function_id)
            return (int16_t)index;
    if (builder->protos.size >= LUAUC_MAX_CLOSURE_COUNT ||
        __luauc_builder_push(builder, &builder->protos, &function_id) == NULL)
        return -1;
    return (int16_t)(builder->protos.size - 1);
}

int luauc_bytecode_set_function_type_info(luauc_bytecode_builder_t* builder, const void* data, size_t size)
{
    luauc_bytecode_function_t* function;
    if (builder == NULL || builder->current_function >= builder->functions.size)
        return 0;
    function = (luauc_bytecode_function_t*)luauc_vector_at(&builder->functions, builder->current_function);
    function->typeinfo.size = 0;
    if (!luauc_buffer_append(&function->typeinfo, data, size))
        return __luauc_builder_fail(builder);
    return 1;
}

void luauc_bytecode_push_local_type_info(
    luauc_bytecode_builder_t* builder, luauc_bytecode_type_t type, uint8_t reg, uint32_t startpc, uint32_t endpc
)
{
    luauc_typed_local_t local;
    local.type = (uint8_t)type;
    local.reg = reg;
    local.startpc = startpc;
    local.endpc = endpc;
    if (builder != NULL)
        __luauc_builder_push(builder, &builder->typed_locals, &local);
}

void luauc_bytecode_push_upval_type_info(luauc_bytecode_builder_t* builder, luauc_bytecode_type_t type)
{
    luauc_typed_upval_t upval;
    upval.type = (uint8_t)type;
    if (builder != NULL)
        __luauc_builder_push(builder, &builder->typed_upvals, &upval);
}

uint32_t luauc_bytecode_add_userdata_type(luauc_bytecode_builder_t* builder, const char* name)
{
    luauc_userdata_type_t type;
    size_t length;

    if (builder == NULL || name == NULL || builder->userdata_types.size >= UINT32_MAX)
        return UINT32_MAX;
    memset(&type, 0, sizeof(type));
    length = strlen(name);
    type.name = (char*)builder->allocator.reallocate(builder->allocator.context, NULL, 0, length + 1);
    if (type.name == NULL)
        return __luauc_builder_fail(builder), UINT32_MAX;
    memcpy(type.name, name, length + 1);
    type.length = length;
    if (__luauc_builder_push(builder, &builder->userdata_types, &type) == NULL)
    {
        builder->allocator.reallocate(builder->allocator.context, type.name, length + 1, 0);
        return UINT32_MAX;
    }
    return (uint32_t)(builder->userdata_types.size - 1);
}

void luauc_bytecode_use_userdata_type(luauc_bytecode_builder_t* builder, uint32_t index)
{
    luauc_userdata_type_t* type;
    assert(builder != NULL && index < builder->userdata_types.size);
    if (builder == NULL || index >= builder->userdata_types.size)
        return;
    type = (luauc_userdata_type_t*)luauc_vector_at(&builder->userdata_types, index);
    type->used = 1;
}

void luauc_bytecode_set_debug_function_name(luauc_bytecode_builder_t* builder, luauc_string_ref_t name)
{
    luauc_bytecode_function_t* function;
    uint32_t index;
    if (builder == NULL || builder->current_function >= builder->functions.size)
        return;
    index = __luauc_add_string(builder, name);
    function = (luauc_bytecode_function_t*)luauc_vector_at(&builder->functions, builder->current_function);
    function->debugname = index;
}

void luauc_bytecode_set_debug_function_line_defined(luauc_bytecode_builder_t* builder, int line)
{
    luauc_bytecode_function_t* function;
    if (builder == NULL || builder->current_function >= builder->functions.size)
        return;
    function = (luauc_bytecode_function_t*)luauc_vector_at(&builder->functions, builder->current_function);
    function->debuglinedefined = line;
}

void luauc_bytecode_set_debug_line(luauc_bytecode_builder_t* builder, int line)
{
    if (builder != NULL)
        builder->debug_line = line;
}

void luauc_bytecode_push_debug_local(
    luauc_bytecode_builder_t* builder, luauc_string_ref_t name, uint8_t reg, uint32_t startpc, uint32_t endpc
)
{
    luauc_debug_local_t local;
    if (builder == NULL)
        return;
    local.name = __luauc_add_string(builder, name);
    local.reg = reg;
    local.startpc = startpc;
    local.endpc = endpc;
    __luauc_builder_push(builder, &builder->debug_locals, &local);
}

void luauc_bytecode_push_debug_upval(luauc_bytecode_builder_t* builder, luauc_string_ref_t name)
{
    luauc_debug_upval_t upval;
    if (builder == NULL)
        return;
    upval.name = __luauc_add_string(builder, name);
    __luauc_builder_push(builder, &builder->debug_upvals, &upval);
}

size_t luauc_bytecode_get_instruction_count(const luauc_bytecode_builder_t* builder)
{
    return builder == NULL ? 0 : builder->instructions.size;
}

size_t luauc_bytecode_get_total_instruction_count(const luauc_bytecode_builder_t* builder)
{
    return builder == NULL ? 0 : builder->total_instruction_count;
}

uint32_t luauc_bytecode_get_debug_pc(const luauc_bytecode_builder_t* builder)
{
    return builder == NULL ? 0 : (uint32_t)builder->instructions.size;
}

int luauc_bytecode_needs_debug_remarks(const luauc_bytecode_builder_t* builder)
{
    (void)builder;
    return 0;
}

void luauc_bytecode_add_debug_remark(luauc_bytecode_builder_t* builder, const char* format, ...)
{
    (void)builder;
    (void)format;
}

static int __luauc_int_log2(int value)
{
    int result = 0;
    assert(value != 0);
    while (value >= (2 << result))
        result++;
    return result;
}

static int __luauc_calc_lines_span(const luauc_bytecode_builder_t* builder)
{
    int span = 1 << 24;
    size_t offset;
    assert(builder->lines.size != 0);

    for (offset = 0; offset < builder->lines.size; offset += (size_t)span)
    {
        size_t next = offset;
        int minimum = *(const int*)luauc_vector_at_const(&builder->lines, offset);
        int maximum = minimum;
        for (; next < builder->lines.size && next < offset + (size_t)span; ++next)
        {
            int line = *(const int*)luauc_vector_at_const(&builder->lines, next);
            if (line < minimum)
                minimum = line;
            if (line > maximum)
                maximum = line;
            if (maximum - minimum > 255)
                break;
        }
        if (next < builder->lines.size && next - offset < (size_t)span)
            span = 1 << __luauc_int_log2((int)(next - offset));
    }
    return span;
}

static int __luauc_write_line_info(luauc_bytecode_builder_t* builder, luauc_buffer_t* output)
{
    int span = __luauc_calc_lines_span(builder);
    int logspan = __luauc_int_log2(span);
    size_t baseline_size = (builder->lines.size - 1) / (size_t)span + 1;
    int* baseline = NULL;
    size_t index;
    uint8_t last_offset = 0;
    int last_line = 0;

    baseline = (int*)builder->allocator.reallocate(
        builder->allocator.context, NULL, 0, baseline_size * sizeof(int)
    );
    if (baseline == NULL)
        return __luauc_builder_fail(builder);

    for (index = 0; index < builder->lines.size; index += (size_t)span)
    {
        size_t next;
        int minimum = *(const int*)luauc_vector_at_const(&builder->lines, index);
        for (next = index; next < builder->lines.size && next < index + (size_t)span; ++next)
        {
            int line = *(const int*)luauc_vector_at_const(&builder->lines, next);
            if (line < minimum)
                minimum = line;
        }
        baseline[index / (size_t)span] = minimum;
    }

    if (!luauc_buffer_append_byte(output, (uint8_t)logspan))
        goto fail;
    for (index = 0; index < builder->lines.size; ++index)
    {
        int line = *(const int*)luauc_vector_at_const(&builder->lines, index);
        int delta = line - baseline[index >> logspan];
        uint8_t offset = (uint8_t)delta;
        assert(delta >= 0 && delta <= 255);
        if (!luauc_buffer_append_byte(output, (uint8_t)(offset - last_offset)))
            goto fail;
        last_offset = offset;
    }
    for (index = 0; index < baseline_size; ++index)
    {
        if (!__luauc_buffer_append_i32(output, baseline[index] - last_line))
            goto fail;
        last_line = baseline[index];
    }

    builder->allocator.reallocate(
        builder->allocator.context, baseline, baseline_size * sizeof(int), 0
    );
    return 1;

fail:
    builder->allocator.reallocate(
        builder->allocator.context, baseline, baseline_size * sizeof(int), 0
    );
    return __luauc_builder_fail(builder);
}

static int __luauc_write_class_shape(luauc_buffer_t* output, const luauc_stored_class_shape_t* shape)
{
    size_t index;
    if (!luauc_buffer_append_varuint(output, (uint32_t)shape->class_name) ||
        !luauc_buffer_append_varuint(output, (uint32_t)shape->property_names.size) ||
        !luauc_buffer_append_varuint(output, (uint32_t)shape->method_names.size))
        return 0;
    for (index = 0; index < shape->property_names.size; ++index)
        if (!luauc_buffer_append_varuint(
                output, (uint32_t)*(const int32_t*)luauc_vector_at_const(&shape->property_names, index)
            ))
            return 0;
    for (index = 0; index < shape->method_names.size; ++index)
        if (!luauc_buffer_append_varuint(
                output, (uint32_t)*(const int32_t*)luauc_vector_at_const(&shape->method_names, index)
            ))
            return 0;
    return 1;
}

static int __luauc_write_function(
    luauc_bytecode_builder_t* builder, luauc_bytecode_function_t* function, uint8_t flags, uint64_t cost
)
{
    luauc_buffer_t* output = &function->data;
    luauc_buffer_t temporary;
    size_t index;
    (void)cost;
    luauc_buffer_init(&temporary, builder->allocator);

    if (!luauc_buffer_append_byte(output, function->maxstacksize) ||
        !luauc_buffer_append_byte(output, function->numparams) ||
        !luauc_buffer_append_byte(output, function->numupvalues) ||
        !luauc_buffer_append_byte(output, function->isvararg) ||
        !luauc_buffer_append_byte(output, flags))
        return __luauc_builder_fail(builder);

    if (function->typeinfo.size != 0 || builder->typed_upvals.size != 0 || builder->typed_locals.size != 0)
    {
        if (!luauc_buffer_append_varuint(&temporary, (uint32_t)function->typeinfo.size) ||
            !luauc_buffer_append_varuint(&temporary, (uint32_t)builder->typed_upvals.size) ||
            !luauc_buffer_append_varuint(&temporary, (uint32_t)builder->typed_locals.size) ||
            !luauc_buffer_append(&temporary, function->typeinfo.data, function->typeinfo.size))
            goto type_fail;
        for (index = 0; index < builder->typed_upvals.size; ++index)
        {
            const luauc_typed_upval_t* upval =
                (const luauc_typed_upval_t*)luauc_vector_at_const(&builder->typed_upvals, index);
            if (!luauc_buffer_append_byte(&temporary, upval->type))
                goto type_fail;
        }
        for (index = 0; index < builder->typed_locals.size; ++index)
        {
            const luauc_typed_local_t* local =
                (const luauc_typed_local_t*)luauc_vector_at_const(&builder->typed_locals, index);
            assert(local->endpc >= local->startpc);
            if (!luauc_buffer_append_byte(&temporary, local->type) ||
                !luauc_buffer_append_byte(&temporary, local->reg) ||
                !luauc_buffer_append_varuint(&temporary, local->startpc) ||
                !luauc_buffer_append_varuint(&temporary, local->endpc - local->startpc))
                goto type_fail;
        }
        if (!luauc_buffer_append_varuint(output, (uint32_t)temporary.size) ||
            !luauc_buffer_append(output, temporary.data, temporary.size))
            goto type_fail;
    }
    else if (!luauc_buffer_append_varuint(output, 0))
        return __luauc_builder_fail(builder);

    luauc_buffer_destroy(&temporary);

    if (!luauc_buffer_append_varuint(output, (uint32_t)builder->instructions.size))
        return __luauc_builder_fail(builder);
    for (index = 0; index < builder->instructions.size; ++index)
        if (!luauc_buffer_append_u32(
                output, *(const uint32_t*)luauc_vector_at_const(&builder->instructions, index)
            ))
            return __luauc_builder_fail(builder);

    if (!luauc_buffer_append_varuint(output, (uint32_t)builder->constants.size))
        return __luauc_builder_fail(builder);
    for (index = 0; index < builder->constants.size; ++index)
    {
        const luauc_constant_t* constant =
            (const luauc_constant_t*)luauc_vector_at_const(&builder->constants, index);
        size_t element;
        switch (constant->type)
        {
        case LUAUC_CONSTANT_NIL:
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_NIL))
                return __luauc_builder_fail(builder);
            break;
        case LUAUC_CONSTANT_BOOLEAN:
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_BOOLEAN) ||
                !luauc_buffer_append_byte(output, (uint8_t)constant->value.boolean_value))
                return __luauc_builder_fail(builder);
            break;
        case LUAUC_CONSTANT_NUMBER:
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_NUMBER) ||
                !__luauc_buffer_append_double(output, constant->value.number_value))
                return __luauc_builder_fail(builder);
            break;
        case LUAUC_CONSTANT_INTEGER:
        {
            uint64_t magnitude;
            int negative = constant->value.integer_value < 0;
            if (negative)
                magnitude = ~(uint64_t)constant->value.integer_value + 1;
            else
                magnitude = (uint64_t)constant->value.integer_value;
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_INTEGER) ||
                !luauc_buffer_append_byte(output, (uint8_t)negative) ||
                !__luauc_buffer_append_u64_varint(output, magnitude))
                return __luauc_builder_fail(builder);
            break;
        }
        case LUAUC_CONSTANT_VECTOR:
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_VECTOR))
                return __luauc_builder_fail(builder);
            for (element = 0; element < 4; ++element)
                if (!__luauc_buffer_append_float(output, constant->value.vector_value[element]))
                    return __luauc_builder_fail(builder);
            break;
        case LUAUC_CONSTANT_STRING:
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_STRING) ||
                !luauc_buffer_append_varuint(output, constant->value.string_value))
                return __luauc_builder_fail(builder);
            break;
        case LUAUC_CONSTANT_IMPORT:
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_IMPORT) ||
                !luauc_buffer_append_u32(output, constant->value.import_value))
                return __luauc_builder_fail(builder);
            break;
        case LUAUC_CONSTANT_TABLE:
        {
            const luauc_table_shape_t* shape = (const luauc_table_shape_t*)luauc_vector_at_const(
                &builder->table_shapes, constant->value.table_value
            );
            if (!luauc_buffer_append_byte(
                    output, (uint8_t)(shape->has_constants ? LBC_CONSTANT_TABLE_WITH_CONSTANTS : LBC_CONSTANT_TABLE)
                ) ||
                !luauc_buffer_append_varuint(output, shape->length))
                return __luauc_builder_fail(builder);
            for (element = 0; element < shape->length; ++element)
            {
                if (!luauc_buffer_append_varuint(output, (uint32_t)shape->keys[element]))
                    return __luauc_builder_fail(builder);
                if (shape->has_constants &&
                    !__luauc_buffer_append_i32(output, shape->constants[element]))
                    return __luauc_builder_fail(builder);
            }
            break;
        }
        case LUAUC_CONSTANT_CLOSURE:
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_CLOSURE) ||
                !luauc_buffer_append_varuint(output, constant->value.closure_value))
                return __luauc_builder_fail(builder);
            break;
        case LUAUC_CONSTANT_CLASS_SHAPE:
        {
            const luauc_stored_class_shape_t* shape = (const luauc_stored_class_shape_t*)luauc_vector_at_const(
                &builder->class_shapes, constant->value.class_shape_value
            );
            if (!luauc_buffer_append_byte(output, LBC_CONSTANT_CLASS_SHAPE) ||
                !__luauc_write_class_shape(output, shape))
                return __luauc_builder_fail(builder);
            break;
        }
        }
    }

    if (!luauc_buffer_append_varuint(output, (uint32_t)builder->protos.size))
        return __luauc_builder_fail(builder);
    for (index = 0; index < builder->protos.size; ++index)
        if (!luauc_buffer_append_varuint(
                output, *(const uint32_t*)luauc_vector_at_const(&builder->protos, index)
            ))
            return __luauc_builder_fail(builder);

    if (!luauc_buffer_append_varuint(output, (uint32_t)function->debuglinedefined) ||
        !luauc_buffer_append_varuint(output, function->debugname))
        return __luauc_builder_fail(builder);

    {
        int has_lines = 1;
        for (index = 0; index < builder->lines.size; ++index)
            if (*(const int*)luauc_vector_at_const(&builder->lines, index) == 0)
            {
                has_lines = 0;
                break;
            }
        if (!luauc_buffer_append_byte(output, (uint8_t)has_lines))
            return __luauc_builder_fail(builder);
        if (has_lines && !__luauc_write_line_info(builder, output))
            return 0;
    }

    if (builder->debug_locals.size != 0 || builder->debug_upvals.size != 0)
    {
        if (!luauc_buffer_append_byte(output, 1) ||
            !luauc_buffer_append_varuint(output, (uint32_t)builder->debug_locals.size))
            return __luauc_builder_fail(builder);
        for (index = 0; index < builder->debug_locals.size; ++index)
        {
            const luauc_debug_local_t* local =
                (const luauc_debug_local_t*)luauc_vector_at_const(&builder->debug_locals, index);
            if (!luauc_buffer_append_varuint(output, local->name) ||
                !luauc_buffer_append_varuint(output, local->startpc) ||
                !luauc_buffer_append_varuint(output, local->endpc) ||
                !luauc_buffer_append_byte(output, local->reg))
                return __luauc_builder_fail(builder);
        }
        if (!luauc_buffer_append_varuint(output, (uint32_t)builder->debug_upvals.size))
            return __luauc_builder_fail(builder);
        for (index = 0; index < builder->debug_upvals.size; ++index)
        {
            const luauc_debug_upval_t* upval =
                (const luauc_debug_upval_t*)luauc_vector_at_const(&builder->debug_upvals, index);
            if (!luauc_buffer_append_varuint(output, upval->name))
                return __luauc_builder_fail(builder);
        }
    }
    else if (!luauc_buffer_append_byte(output, 0))
        return __luauc_builder_fail(builder);

    luauc_buffer_destroy(&temporary);
    return 1;

type_fail:
    luauc_buffer_destroy(&temporary);
    return __luauc_builder_fail(builder);
}

int luauc_bytecode_end_function(
    luauc_bytecode_builder_t* builder, uint8_t maxstacksize, uint8_t numupvalues, uint8_t flags, uint64_t cost
)
{
    luauc_bytecode_function_t* function;
    if (builder == NULL || builder->current_function >= builder->functions.size)
        return 0;
    function = (luauc_bytecode_function_t*)luauc_vector_at(&builder->functions, builder->current_function);
    function->maxstacksize = maxstacksize;
    function->numupvalues = numupvalues;
    if (!__luauc_write_function(builder, function, flags, cost))
        return 0;
    builder->current_function = UINT32_MAX;
    builder->total_instruction_count += builder->instructions.size;
    __luauc_clear_function_state(builder);
    return !builder->failed;
}

static int __luauc_jump_compare(const void* left, const void* right)
{
    const luauc_jump_t* a = (const luauc_jump_t*)left;
    const luauc_jump_t* b = (const luauc_jump_t*)right;
    return a->source < b->source ? -1 : a->source > b->source;
}

void luauc_bytecode_fold_jumps(luauc_bytecode_builder_t* builder)
{
    size_t index;
    if (builder == NULL || builder->has_long_jumps)
        return;
    for (index = 0; index < builder->jumps.size; ++index)
    {
        luauc_jump_t* jump = (luauc_jump_t*)luauc_vector_at(&builder->jumps, index);
        uint32_t* instructions = (uint32_t*)builder->instructions.data;
        uint32_t jump_label = jump->source;
        uint32_t jump_instruction = instructions[jump_label];
        uint32_t target_label = jump_label + 1u + (uint32_t)LUAU_INSN_D(jump_instruction);
        uint32_t target_instruction = instructions[target_label];
        int64_t offset;

        while (LUAU_INSN_OP(target_instruction) == LOP_JUMP && LUAU_INSN_D(target_instruction) >= 0)
        {
            target_label = target_label + 1u + (uint32_t)LUAU_INSN_D(target_instruction);
            target_instruction = instructions[target_label];
        }
        offset = (int64_t)target_label - (int64_t)jump_label - 1;
        if (LUAU_INSN_OP(jump_instruction) == LOP_JUMP && LUAU_INSN_OP(target_instruction) == LOP_RETURN)
            instructions[jump_label] = target_instruction;
        else if (offset >= INT16_MIN && offset <= INT16_MAX)
            instructions[jump_label] = (instructions[jump_label] & 0xffffu) |
                ((uint32_t)(uint16_t)(int16_t)offset << 16);
        jump->target = target_label;
    }
}

int luauc_bytecode_expand_jumps(luauc_bytecode_builder_t* builder)
{
    const int maximum_conservative_distance = INT16_MAX / 3;
    luauc_vector_t remap;
    luauc_vector_t new_instructions;
    luauc_vector_t new_lines;
    size_t current_jump = 0;
    size_t pending = 0;
    size_t index;

    if (builder == NULL)
        return 0;
    if (!builder->has_long_jumps)
        return 1;

    qsort(builder->jumps.data, builder->jumps.size, sizeof(luauc_jump_t), __luauc_jump_compare);
    luauc_vector_init(&remap, sizeof(uint32_t), builder->allocator);
    luauc_vector_init(&new_instructions, sizeof(uint32_t), builder->allocator);
    luauc_vector_init(&new_lines, sizeof(int), builder->allocator);
    if (!luauc_vector_reserve(&remap, builder->instructions.size) ||
        !luauc_vector_reserve(&new_instructions, builder->instructions.size) ||
        !luauc_vector_reserve(&new_lines, builder->lines.size))
        goto fail;
    remap.size = builder->instructions.size;

    for (index = 0; index < builder->instructions.size;)
    {
        uint32_t instruction = *(const uint32_t*)luauc_vector_at_const(&builder->instructions, index);
        luauc_opcode_t op = (luauc_opcode_t)LUAU_INSN_OP(instruction);
        int op_length;
        int word;

        if (current_jump < builder->jumps.size)
        {
            const luauc_jump_t* jump = (const luauc_jump_t*)luauc_vector_at_const(&builder->jumps, current_jump);
            if (jump->source == index)
            {
                int64_t offset = (int64_t)jump->target - (int64_t)jump->source - 1;
                if (llabs(offset) > maximum_conservative_distance)
                {
                    uint32_t trampoline_jump = (uint32_t)LOP_JUMP | (1u << 16);
                    uint32_t trampoline_long = (uint32_t)LOP_JUMPX;
                    int line = *(const int*)luauc_vector_at_const(&builder->lines, index);
                    if (luauc_vector_push(&new_instructions, &trampoline_jump) == NULL ||
                        luauc_vector_push(&new_instructions, &trampoline_long) == NULL ||
                        luauc_vector_push(&new_lines, &line) == NULL ||
                        luauc_vector_push(&new_lines, &line) == NULL)
                        goto fail;
                    pending++;
                }
                current_jump++;
            }
        }

        op_length = __luauc_get_op_length(op);
        for (word = 0; word < op_length; ++word)
        {
            uint32_t* mapped = (uint32_t*)luauc_vector_at(&remap, index);
            int line = *(const int*)luauc_vector_at_const(&builder->lines, index);
            instruction = *(const uint32_t*)luauc_vector_at_const(&builder->instructions, index);
            *mapped = (uint32_t)new_instructions.size;
            if (luauc_vector_push(&new_instructions, &instruction) == NULL ||
                luauc_vector_push(&new_lines, &line) == NULL)
                goto fail;
            index++;
        }
    }

    for (index = 0; index < builder->jumps.size; ++index)
    {
        const luauc_jump_t* jump = (const luauc_jump_t*)luauc_vector_at_const(&builder->jumps, index);
        uint32_t source = *(const uint32_t*)luauc_vector_at_const(&remap, jump->source);
        uint32_t target = *(const uint32_t*)luauc_vector_at_const(&remap, jump->target);
        int64_t old_offset = (int64_t)jump->target - (int64_t)jump->source - 1;
        int64_t new_offset = (int64_t)target - (int64_t)source - 1;

        if (llabs(old_offset) > maximum_conservative_distance)
        {
            uint32_t* trampoline = (uint32_t*)luauc_vector_at(&new_instructions, source - 1);
            uint32_t* instruction = (uint32_t*)luauc_vector_at(&new_instructions, source);
            *trampoline = (*trampoline & 0xffu) | ((uint32_t)(new_offset + 1) << 8);
            *instruction = (*instruction & 0xffffu) | ((uint32_t)(uint16_t)-2 << 16);
            pending--;
        }
        else
        {
            uint32_t* instruction = (uint32_t*)luauc_vector_at(&new_instructions, source);
            assert(new_offset >= INT16_MIN && new_offset <= INT16_MAX);
            *instruction = (*instruction & 0xffffu) | ((uint32_t)(uint16_t)(int16_t)new_offset << 16);
        }
    }
    assert(pending == 0);
    (void)pending;

    for (index = 0; index < builder->debug_locals.size; ++index)
    {
        luauc_debug_local_t* local = (luauc_debug_local_t*)luauc_vector_at(&builder->debug_locals, index);
        if (local->startpc != local->endpc)
            local->endpc = *(uint32_t*)luauc_vector_at(&remap, local->endpc - 1) + 1;
        else
            local->endpc = *(uint32_t*)luauc_vector_at(&remap, local->endpc);
        local->startpc = *(uint32_t*)luauc_vector_at(&remap, local->startpc);
    }
    for (index = 0; index < builder->typed_locals.size; ++index)
    {
        luauc_typed_local_t* local = (luauc_typed_local_t*)luauc_vector_at(&builder->typed_locals, index);
        if (local->startpc != local->endpc)
            local->endpc = *(uint32_t*)luauc_vector_at(&remap, local->endpc - 1) + 1;
        else
            local->endpc = *(uint32_t*)luauc_vector_at(&remap, local->endpc);
        local->startpc = *(uint32_t*)luauc_vector_at(&remap, local->startpc);
    }

    luauc_vector_destroy(&builder->instructions);
    luauc_vector_destroy(&builder->lines);
    builder->instructions = new_instructions;
    builder->lines = new_lines;
    luauc_vector_destroy(&remap);
    return 1;

fail:
    luauc_vector_destroy(&remap);
    luauc_vector_destroy(&new_instructions);
    luauc_vector_destroy(&new_lines);
    return __luauc_builder_fail(builder);
}

int luauc_bytecode_finalize(luauc_bytecode_builder_t* builder)
{
    size_t index;
    if (builder == NULL || builder->bytecode.size != 0 || builder->current_function != UINT32_MAX ||
        builder->main_function >= builder->functions.size)
        return 0;

    for (index = 0; index < builder->userdata_types.size; ++index)
    {
        luauc_userdata_type_t* type = (luauc_userdata_type_t*)luauc_vector_at(&builder->userdata_types, index);
        if (type->used)
        {
            luauc_string_ref_t reference;
            reference.data = type->name;
            reference.length = type->length;
            type->name_ref = __luauc_add_string(builder, reference);
            if (type->name_ref == 0)
                return __luauc_builder_fail(builder);
        }
    }

    if (!luauc_buffer_append_byte(&builder->bytecode, luauc_bytecode_get_version()) ||
        !luauc_buffer_append_byte(&builder->bytecode, luauc_bytecode_get_type_encoding_version()) ||
        !luauc_buffer_append_varuint(&builder->bytecode, (uint32_t)builder->strings.size))
        return __luauc_builder_fail(builder);

    for (index = 0; index < builder->strings.size; ++index)
    {
        const luauc_string_ref_t* string = (const luauc_string_ref_t*)luauc_vector_at_const(&builder->strings, index);
        if (!luauc_buffer_append_varuint(&builder->bytecode, (uint32_t)string->length) ||
            !luauc_buffer_append(&builder->bytecode, string->data, string->length))
            return __luauc_builder_fail(builder);
    }

    for (index = 0; index < builder->userdata_types.size; ++index)
    {
        const luauc_userdata_type_t* type =
            (const luauc_userdata_type_t*)luauc_vector_at_const(&builder->userdata_types, index);
        if (type->used &&
            (!luauc_buffer_append_byte(&builder->bytecode, (uint8_t)(index + 1)) ||
             !luauc_buffer_append_varuint(&builder->bytecode, type->name_ref)))
            return __luauc_builder_fail(builder);
    }
    if (!luauc_buffer_append_byte(&builder->bytecode, 0) ||
        !luauc_buffer_append_varuint(&builder->bytecode, (uint32_t)builder->functions.size))
        return __luauc_builder_fail(builder);

    for (index = 0; index < builder->functions.size; ++index)
    {
        const luauc_bytecode_function_t* function =
            (const luauc_bytecode_function_t*)luauc_vector_at_const(&builder->functions, index);
        if (!luauc_buffer_append(&builder->bytecode, function->data.data, function->data.size))
            return __luauc_builder_fail(builder);
    }
    if (!luauc_buffer_append_varuint(&builder->bytecode, builder->main_function))
        return __luauc_builder_fail(builder);
    return !builder->failed;
}

unsigned char* luauc_bytecode_release(luauc_bytecode_builder_t* builder, size_t* size)
{
    return builder == NULL ? NULL : luauc_buffer_release(&builder->bytecode, size);
}

uint32_t luauc_bytecode_get_import_id1(int32_t id0)
{
    assert((uint32_t)id0 < 1024);
    return (1u << 30) | ((uint32_t)id0 << 20);
}

uint32_t luauc_bytecode_get_import_id2(int32_t id0, int32_t id1)
{
    assert((uint32_t)(id0 | id1) < 1024);
    return (2u << 30) | ((uint32_t)id0 << 20) | ((uint32_t)id1 << 10);
}

uint32_t luauc_bytecode_get_import_id3(int32_t id0, int32_t id1, int32_t id2)
{
    assert((uint32_t)(id0 | id1 | id2) < 1024);
    return (3u << 30) | ((uint32_t)id0 << 20) | ((uint32_t)id1 << 10) | (uint32_t)id2;
}

uint32_t luauc_bytecode_get_string_hash(luauc_string_ref_t key)
{
    uint32_t hash = (uint32_t)key.length;
    size_t index;
    for (index = key.length; index > 0; --index)
        hash ^= (hash << 5) + (hash >> 2) + (uint8_t)key.data[index - 1];
    return hash;
}

unsigned char* luauc_bytecode_get_error(const char* message, size_t message_size, size_t* result_size)
{
    unsigned char* result;
    if (message == NULL && message_size != 0)
        return NULL;
    result = (unsigned char*)malloc(message_size + 1);
    if (result == NULL)
        return NULL;
    result[0] = 0;
    if (message_size != 0)
        memcpy(result + 1, message, message_size);
    if (result_size != NULL)
        *result_size = message_size + 1;
    return result;
}

uint8_t luauc_bytecode_get_version(void)
{
    return LBC_VERSION_TARGET;
}

uint8_t luauc_bytecode_get_type_encoding_version(void)
{
    return LBC_TYPE_VERSION_TARGET;
}
