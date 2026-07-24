// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#ifndef LUAUC_COMMON_H
#define LUAUC_COMMON_H

#include <stddef.h>
#include <stdint.h>

typedef struct luauc_allocator_t
{
    void* context;
    void* (*reallocate)(void* context, void* pointer, size_t old_size, size_t new_size);
} luauc_allocator_t;

typedef struct luauc_vector_t
{
    unsigned char* data;
    size_t size;
    size_t capacity;
    size_t element_size;
    luauc_allocator_t allocator;
} luauc_vector_t;

typedef struct luauc_buffer_t
{
    unsigned char* data;
    size_t size;
    size_t capacity;
    luauc_allocator_t allocator;
} luauc_buffer_t;

typedef struct luauc_arena_block_t luauc_arena_block_t;

typedef struct luauc_arena_t
{
    luauc_arena_block_t* head;
    size_t block_size;
    luauc_allocator_t allocator;
} luauc_arena_t;

typedef struct luauc_string_map_entry_t
{
    const char* key;
    size_t key_length;
    size_t value;
    uint64_t hash;
} luauc_string_map_entry_t;

typedef struct luauc_string_map_t
{
    luauc_string_map_entry_t* entries;
    size_t size;
    size_t capacity;
    luauc_allocator_t allocator;
} luauc_string_map_t;

luauc_allocator_t luauc_default_allocator(void);

int luauc_size_add(size_t left, size_t right, size_t* result);
int luauc_size_multiply(size_t left, size_t right, size_t* result);
size_t luauc_align_up(size_t value, size_t alignment);

int luauc_vector_init(luauc_vector_t* vector, size_t element_size, luauc_allocator_t allocator);
void luauc_vector_destroy(luauc_vector_t* vector);
int luauc_vector_reserve(luauc_vector_t* vector, size_t capacity);
void* luauc_vector_push(luauc_vector_t* vector, const void* value);
void* luauc_vector_at(luauc_vector_t* vector, size_t index);
const void* luauc_vector_at_const(const luauc_vector_t* vector, size_t index);
void luauc_vector_clear(luauc_vector_t* vector);

void luauc_buffer_init(luauc_buffer_t* buffer, luauc_allocator_t allocator);
void luauc_buffer_destroy(luauc_buffer_t* buffer);
int luauc_buffer_reserve(luauc_buffer_t* buffer, size_t capacity);
int luauc_buffer_append(luauc_buffer_t* buffer, const void* data, size_t size);
int luauc_buffer_append_byte(luauc_buffer_t* buffer, uint8_t value);
int luauc_buffer_append_u32(luauc_buffer_t* buffer, uint32_t value);
int luauc_buffer_append_varuint(luauc_buffer_t* buffer, uint32_t value);
unsigned char* luauc_buffer_release(luauc_buffer_t* buffer, size_t* size);

void luauc_arena_init(luauc_arena_t* arena, size_t block_size, luauc_allocator_t allocator);
void luauc_arena_destroy(luauc_arena_t* arena);
void* luauc_arena_allocate(luauc_arena_t* arena, size_t size, size_t alignment);
char* luauc_arena_duplicate(luauc_arena_t* arena, const char* text, size_t length);

void luauc_string_map_init(luauc_string_map_t* map, luauc_allocator_t allocator);
void luauc_string_map_destroy(luauc_string_map_t* map);
int luauc_string_map_reserve(luauc_string_map_t* map, size_t capacity);
int luauc_string_map_insert(luauc_string_map_t* map, const char* key, size_t key_length, size_t value, int* inserted);
int luauc_string_map_find(const luauc_string_map_t* map, const char* key, size_t key_length, size_t* value);
uint64_t luauc_hash_bytes(const void* data, size_t size);

#endif
