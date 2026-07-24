// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#ifndef LUAUC_BYTECODE_BUILDER_H
#define LUAUC_BYTECODE_BUILDER_H

#include "luauc_bytecode.h"
#include "common/luauc_common.h"

#include <stddef.h>
#include <stdint.h>

typedef struct luauc_string_ref_t
{
    const char* data;
    size_t length;
} luauc_string_ref_t;

enum
{
    LUAUC_TABLE_SHAPE_MAX_LENGTH = 32
};

typedef struct luauc_table_shape_t
{
    int32_t keys[LUAUC_TABLE_SHAPE_MAX_LENGTH];
    int32_t constants[LUAUC_TABLE_SHAPE_MAX_LENGTH];
    unsigned int length;
    int has_constants;
} luauc_table_shape_t;

typedef struct luauc_class_shape_t
{
    int32_t class_name;
    const int32_t* property_names;
    size_t property_count;
    const int32_t* method_names;
    size_t method_count;
} luauc_class_shape_t;

typedef struct luauc_bytecode_builder_t luauc_bytecode_builder_t;

struct luauc_bytecode_builder_t
{
    luauc_allocator_t allocator;
    luauc_vector_t functions;
    uint32_t current_function;
    uint32_t main_function;
    size_t total_instruction_count;

    luauc_vector_t instructions;
    luauc_vector_t lines;
    luauc_vector_t constants;
    luauc_vector_t protos;
    luauc_vector_t jumps;
    luauc_vector_t table_shapes;
    luauc_vector_t class_shapes;
    luauc_vector_t feedback_slots;
    luauc_vector_t debug_locals;
    luauc_vector_t debug_upvals;
    luauc_vector_t typed_locals;
    luauc_vector_t typed_upvals;
    luauc_vector_t userdata_types;
    luauc_vector_t strings;

    int has_long_jumps;
    int debug_line;
    int failed;
    luauc_buffer_t bytecode;
};

int luauc_bytecode_builder_init(luauc_bytecode_builder_t* builder, luauc_allocator_t allocator);
void luauc_bytecode_builder_destroy(luauc_bytecode_builder_t* builder);

uint32_t luauc_bytecode_begin_function(luauc_bytecode_builder_t* builder, uint8_t numparams, int isvararg);
int luauc_bytecode_end_function(luauc_bytecode_builder_t* builder, uint8_t maxstacksize, uint8_t numupvalues, uint8_t flags, uint64_t cost);
void luauc_bytecode_set_main_function(luauc_bytecode_builder_t* builder, uint32_t function_id);

int32_t luauc_bytecode_add_constant_nil(luauc_bytecode_builder_t* builder);
int32_t luauc_bytecode_add_constant_boolean(luauc_bytecode_builder_t* builder, int value);
int32_t luauc_bytecode_add_constant_number(luauc_bytecode_builder_t* builder, double value);
int32_t luauc_bytecode_add_constant_integer(luauc_bytecode_builder_t* builder, int64_t value);
int32_t luauc_bytecode_add_constant_vector(luauc_bytecode_builder_t* builder, float x, float y, float z, float w);
int32_t luauc_bytecode_add_constant_string(luauc_bytecode_builder_t* builder, luauc_string_ref_t value);
int32_t luauc_bytecode_add_import(luauc_bytecode_builder_t* builder, uint32_t import_id);
int32_t luauc_bytecode_add_constant_table(luauc_bytecode_builder_t* builder, const luauc_table_shape_t* shape);
int32_t luauc_bytecode_add_constant_closure(luauc_bytecode_builder_t* builder, uint32_t function_id);
int32_t luauc_bytecode_add_class_shape(luauc_bytecode_builder_t* builder, const luauc_class_shape_t* shape);
uint32_t luauc_bytecode_add_feedback_slot(luauc_bytecode_builder_t* builder, luauc_feedback_type_t type);
int16_t luauc_bytecode_add_child_function(luauc_bytecode_builder_t* builder, uint32_t function_id);

void luauc_bytecode_emit_abc(luauc_bytecode_builder_t* builder, luauc_opcode_t op, uint8_t a, uint8_t b, uint8_t c);
void luauc_bytecode_emit_ad(luauc_bytecode_builder_t* builder, luauc_opcode_t op, uint8_t a, int16_t d);
void luauc_bytecode_emit_e(luauc_bytecode_builder_t* builder, luauc_opcode_t op, int32_t e);
void luauc_bytecode_emit_aux(luauc_bytecode_builder_t* builder, uint32_t aux);
void luauc_bytecode_undo_emit(luauc_bytecode_builder_t* builder, luauc_opcode_t op);
size_t luauc_bytecode_emit_label(const luauc_bytecode_builder_t* builder);
int luauc_bytecode_patch_jump_d(luauc_bytecode_builder_t* builder, size_t jump_label, size_t target_label);
int luauc_bytecode_patch_skip_c(luauc_bytecode_builder_t* builder, size_t jump_label, size_t target_label);
void luauc_bytecode_patch_aux(luauc_bytecode_builder_t* builder, size_t target_aux, int32_t new_value);
void luauc_bytecode_fold_jumps(luauc_bytecode_builder_t* builder);
int luauc_bytecode_expand_jumps(luauc_bytecode_builder_t* builder);

int luauc_bytecode_set_function_type_info(luauc_bytecode_builder_t* builder, const void* data, size_t size);
void luauc_bytecode_push_local_type_info(
    luauc_bytecode_builder_t* builder, luauc_bytecode_type_t type, uint8_t reg, uint32_t startpc, uint32_t endpc
);
void luauc_bytecode_push_upval_type_info(luauc_bytecode_builder_t* builder, luauc_bytecode_type_t type);
uint32_t luauc_bytecode_add_userdata_type(luauc_bytecode_builder_t* builder, const char* name);
void luauc_bytecode_use_userdata_type(luauc_bytecode_builder_t* builder, uint32_t index);

void luauc_bytecode_set_debug_function_name(luauc_bytecode_builder_t* builder, luauc_string_ref_t name);
void luauc_bytecode_set_debug_function_line_defined(luauc_bytecode_builder_t* builder, int line);
void luauc_bytecode_set_debug_line(luauc_bytecode_builder_t* builder, int line);
void luauc_bytecode_push_debug_local(
    luauc_bytecode_builder_t* builder, luauc_string_ref_t name, uint8_t reg, uint32_t startpc, uint32_t endpc
);
void luauc_bytecode_push_debug_upval(luauc_bytecode_builder_t* builder, luauc_string_ref_t name);

size_t luauc_bytecode_get_instruction_count(const luauc_bytecode_builder_t* builder);
size_t luauc_bytecode_get_total_instruction_count(const luauc_bytecode_builder_t* builder);
uint32_t luauc_bytecode_get_debug_pc(const luauc_bytecode_builder_t* builder);
int luauc_bytecode_needs_debug_remarks(const luauc_bytecode_builder_t* builder);
void luauc_bytecode_add_debug_remark(luauc_bytecode_builder_t* builder, const char* format, ...);

int luauc_bytecode_finalize(luauc_bytecode_builder_t* builder);
unsigned char* luauc_bytecode_release(luauc_bytecode_builder_t* builder, size_t* size);

uint32_t luauc_bytecode_get_import_id1(int32_t id0);
uint32_t luauc_bytecode_get_import_id2(int32_t id0, int32_t id1);
uint32_t luauc_bytecode_get_import_id3(int32_t id0, int32_t id1, int32_t id2);
uint32_t luauc_bytecode_get_string_hash(luauc_string_ref_t key);
unsigned char* luauc_bytecode_get_error(const char* message, size_t message_size, size_t* result_size);
uint8_t luauc_bytecode_get_version(void);
uint8_t luauc_bytecode_get_type_encoding_version(void);

#endif
