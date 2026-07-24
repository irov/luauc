// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#include "common/luauc_common.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct luauc_arena_block_t
{
    luauc_arena_block_t* next;
    size_t size;
    size_t used;
    unsigned char data[];
};

static void* __luauc_default_reallocate(void* context, void* pointer, size_t old_size, size_t new_size)
{
    (void)context;
    (void)old_size;

    if (new_size == 0)
    {
        free(pointer);
        return NULL;
    }

    return realloc(pointer, new_size);
}

luauc_allocator_t luauc_default_allocator(void)
{
    luauc_allocator_t allocator;
    allocator.context = NULL;
    allocator.reallocate = __luauc_default_reallocate;
    return allocator;
}

static luauc_allocator_t __luauc_allocator_or_default(luauc_allocator_t allocator)
{
    if (allocator.reallocate == NULL)
        return luauc_default_allocator();

    return allocator;
}

int luauc_size_add(size_t left, size_t right, size_t* result)
{
    if (result == NULL || left > SIZE_MAX - right)
        return 0;

    *result = left + right;
    return 1;
}

int luauc_size_multiply(size_t left, size_t right, size_t* result)
{
    if (result == NULL || (right != 0 && left > SIZE_MAX / right))
        return 0;

    *result = left * right;
    return 1;
}

size_t luauc_align_up(size_t value, size_t alignment)
{
    size_t remainder;

    if (alignment == 0)
        return value;

    remainder = value % alignment;
    if (remainder == 0)
        return value;

    if (value > SIZE_MAX - (alignment - remainder))
        return SIZE_MAX;

    return value + alignment - remainder;
}

int luauc_vector_init(luauc_vector_t* vector, size_t element_size, luauc_allocator_t allocator)
{
    if (vector == NULL || element_size == 0)
        return 0;

    vector->data = NULL;
    vector->size = 0;
    vector->capacity = 0;
    vector->element_size = element_size;
    vector->allocator = __luauc_allocator_or_default(allocator);
    return 1;
}

void luauc_vector_destroy(luauc_vector_t* vector)
{
    size_t old_size;

    if (vector == NULL)
        return;

    old_size = vector->capacity * vector->element_size;
    if (vector->data != NULL)
        vector->allocator.reallocate(vector->allocator.context, vector->data, old_size, 0);

    vector->data = NULL;
    vector->size = 0;
    vector->capacity = 0;
}

int luauc_vector_reserve(luauc_vector_t* vector, size_t capacity)
{
    size_t old_bytes;
    size_t new_bytes;
    size_t next_capacity;
    void* data;

    if (vector == NULL)
        return 0;

    if (capacity <= vector->capacity)
        return 1;

    next_capacity = vector->capacity == 0 ? 8 : vector->capacity;
    while (next_capacity < capacity)
    {
        if (next_capacity > SIZE_MAX / 2)
        {
            next_capacity = capacity;
            break;
        }

        next_capacity *= 2;
    }

    if (!luauc_size_multiply(vector->capacity, vector->element_size, &old_bytes) ||
        !luauc_size_multiply(next_capacity, vector->element_size, &new_bytes))
        return 0;

    data = vector->allocator.reallocate(vector->allocator.context, vector->data, old_bytes, new_bytes);
    if (data == NULL)
        return 0;

    vector->data = (unsigned char*)data;
    vector->capacity = next_capacity;
    return 1;
}

void* luauc_vector_push(luauc_vector_t* vector, const void* value)
{
    unsigned char* destination;

    if (vector == NULL || !luauc_vector_reserve(vector, vector->size + 1))
        return NULL;

    destination = vector->data + vector->size * vector->element_size;
    if (value != NULL)
        memcpy(destination, value, vector->element_size);
    else
        memset(destination, 0, vector->element_size);

    vector->size++;
    return destination;
}

void* luauc_vector_at(luauc_vector_t* vector, size_t index)
{
    if (vector == NULL || index >= vector->size)
        return NULL;

    return vector->data + index * vector->element_size;
}

const void* luauc_vector_at_const(const luauc_vector_t* vector, size_t index)
{
    if (vector == NULL || index >= vector->size)
        return NULL;

    return vector->data + index * vector->element_size;
}

void luauc_vector_clear(luauc_vector_t* vector)
{
    if (vector != NULL)
        vector->size = 0;
}

void luauc_buffer_init(luauc_buffer_t* buffer, luauc_allocator_t allocator)
{
    if (buffer == NULL)
        return;

    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
    buffer->allocator = __luauc_allocator_or_default(allocator);
}

void luauc_buffer_destroy(luauc_buffer_t* buffer)
{
    if (buffer == NULL)
        return;

    if (buffer->data != NULL)
        buffer->allocator.reallocate(buffer->allocator.context, buffer->data, buffer->capacity, 0);

    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

int luauc_buffer_reserve(luauc_buffer_t* buffer, size_t capacity)
{
    size_t next_capacity;
    void* data;

    if (buffer == NULL)
        return 0;

    if (capacity <= buffer->capacity)
        return 1;

    next_capacity = buffer->capacity == 0 ? 64 : buffer->capacity;
    while (next_capacity < capacity)
    {
        if (next_capacity > SIZE_MAX / 2)
        {
            next_capacity = capacity;
            break;
        }

        next_capacity *= 2;
    }

    data = buffer->allocator.reallocate(buffer->allocator.context, buffer->data, buffer->capacity, next_capacity);
    if (data == NULL)
        return 0;

    buffer->data = (unsigned char*)data;
    buffer->capacity = next_capacity;
    return 1;
}

int luauc_buffer_append(luauc_buffer_t* buffer, const void* data, size_t size)
{
    size_t next_size;

    if (buffer == NULL || (size != 0 && data == NULL) || !luauc_size_add(buffer->size, size, &next_size))
        return 0;

    if (!luauc_buffer_reserve(buffer, next_size))
        return 0;

    if (size != 0)
        memcpy(buffer->data + buffer->size, data, size);

    buffer->size = next_size;
    return 1;
}

int luauc_buffer_append_byte(luauc_buffer_t* buffer, uint8_t value)
{
    return luauc_buffer_append(buffer, &value, sizeof(value));
}

int luauc_buffer_append_u32(luauc_buffer_t* buffer, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
    return luauc_buffer_append(buffer, bytes, sizeof(bytes));
}

int luauc_buffer_append_varuint(luauc_buffer_t* buffer, uint32_t value)
{
    do
    {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7;
        if (value != 0)
            byte = (uint8_t)(byte | 0x80u);

        if (!luauc_buffer_append_byte(buffer, byte))
            return 0;
    } while (value != 0);

    return 1;
}

unsigned char* luauc_buffer_release(luauc_buffer_t* buffer, size_t* size)
{
    unsigned char* data;

    if (buffer == NULL)
        return NULL;

    data = buffer->data;
    if (size != NULL)
        *size = buffer->size;

    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
    return data;
}

void luauc_arena_init(luauc_arena_t* arena, size_t block_size, luauc_allocator_t allocator)
{
    if (arena == NULL)
        return;

    arena->head = NULL;
    arena->block_size = block_size == 0 ? 4096 : block_size;
    arena->allocator = __luauc_allocator_or_default(allocator);
}

void luauc_arena_destroy(luauc_arena_t* arena)
{
    luauc_arena_block_t* block;

    if (arena == NULL)
        return;

    block = arena->head;
    while (block != NULL)
    {
        luauc_arena_block_t* next = block->next;
        size_t allocation_size = sizeof(luauc_arena_block_t) + block->size;
        arena->allocator.reallocate(arena->allocator.context, block, allocation_size, 0);
        block = next;
    }

    arena->head = NULL;
}

void* luauc_arena_allocate(luauc_arena_t* arena, size_t size, size_t alignment)
{
    luauc_arena_block_t* block;
    size_t offset;

    if (arena == NULL || size == 0 || alignment == 0)
        return NULL;

    block = arena->head;
    if (block != NULL)
    {
        offset = luauc_align_up(block->used, alignment);
        if (offset != SIZE_MAX && offset <= block->size && size <= block->size - offset)
        {
            block->used = offset + size;
            return block->data + offset;
        }
    }

    {
        size_t required;
        size_t block_size;
        size_t allocation_size;

        if (!luauc_size_add(size, alignment - 1, &required))
            return NULL;

        block_size = arena->block_size > required ? arena->block_size : required;
        if (!luauc_size_add(sizeof(luauc_arena_block_t), block_size, &allocation_size))
            return NULL;

        block = (luauc_arena_block_t*)arena->allocator.reallocate(arena->allocator.context, NULL, 0, allocation_size);
        if (block == NULL)
            return NULL;

        block->next = arena->head;
        block->size = block_size;
        block->used = 0;
        arena->head = block;
    }

    offset = luauc_align_up(block->used, alignment);
    if (offset == SIZE_MAX || offset > block->size || size > block->size - offset)
        return NULL;

    block->used = offset + size;
    return block->data + offset;
}

char* luauc_arena_duplicate(luauc_arena_t* arena, const char* text, size_t length)
{
    char* result;

    if (text == NULL && length != 0)
        return NULL;

    result = (char*)luauc_arena_allocate(arena, length + 1, 1);
    if (result == NULL)
        return NULL;

    if (length != 0)
        memcpy(result, text, length);
    result[length] = '\0';
    return result;
}

uint64_t luauc_hash_bytes(const void* data, size_t size)
{
    const unsigned char* bytes = (const unsigned char*)data;
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;

    for (index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }

    return hash == 0 ? 1 : hash;
}

void luauc_string_map_init(luauc_string_map_t* map, luauc_allocator_t allocator)
{
    if (map == NULL)
        return;

    map->entries = NULL;
    map->size = 0;
    map->capacity = 0;
    map->allocator = __luauc_allocator_or_default(allocator);
}

void luauc_string_map_destroy(luauc_string_map_t* map)
{
    if (map == NULL)
        return;

    if (map->entries != NULL)
    {
        size_t bytes = map->capacity * sizeof(luauc_string_map_entry_t);
        map->allocator.reallocate(map->allocator.context, map->entries, bytes, 0);
    }

    map->entries = NULL;
    map->size = 0;
    map->capacity = 0;
}

static int __luauc_string_map_insert_entry(luauc_string_map_entry_t* entries, size_t capacity, luauc_string_map_entry_t entry)
{
    size_t index;
    size_t mask;

    if (entries == NULL || capacity == 0 || (capacity & (capacity - 1)) != 0)
        return 0;

    mask = capacity - 1;
    index = (size_t)entry.hash & mask;
    while (entries[index].hash != 0)
        index = (index + 1) & mask;

    entries[index] = entry;
    return 1;
}

int luauc_string_map_reserve(luauc_string_map_t* map, size_t capacity)
{
    size_t next_capacity;
    size_t bytes;
    luauc_string_map_entry_t* entries;
    size_t index;

    if (map == NULL)
        return 0;

    next_capacity = map->capacity == 0 ? 16 : map->capacity;
    while (next_capacity * 3 / 4 < capacity)
    {
        if (next_capacity > SIZE_MAX / 2)
            return 0;
        next_capacity *= 2;
    }

    if (next_capacity == map->capacity)
        return 1;

    if (!luauc_size_multiply(next_capacity, sizeof(luauc_string_map_entry_t), &bytes))
        return 0;

    entries = (luauc_string_map_entry_t*)map->allocator.reallocate(map->allocator.context, NULL, 0, bytes);
    if (entries == NULL)
        return 0;
    memset(entries, 0, bytes);

    for (index = 0; index < map->capacity; ++index)
    {
        if (map->entries[index].hash != 0)
            __luauc_string_map_insert_entry(entries, next_capacity, map->entries[index]);
    }

    if (map->entries != NULL)
    {
        size_t old_bytes = map->capacity * sizeof(luauc_string_map_entry_t);
        map->allocator.reallocate(map->allocator.context, map->entries, old_bytes, 0);
    }

    map->entries = entries;
    map->capacity = next_capacity;
    return 1;
}

int luauc_string_map_insert(luauc_string_map_t* map, const char* key, size_t key_length, size_t value, int* inserted)
{
    uint64_t hash;
    size_t index;
    size_t mask;

    if (map == NULL || (key == NULL && key_length != 0))
        return 0;

    if (!luauc_string_map_reserve(map, map->size + 1))
        return 0;

    hash = luauc_hash_bytes(key, key_length);
    mask = map->capacity - 1;
    index = (size_t)hash & mask;

    while (map->entries[index].hash != 0)
    {
        if (map->entries[index].hash == hash && map->entries[index].key_length == key_length &&
            memcmp(map->entries[index].key, key, key_length) == 0)
        {
            map->entries[index].value = value;
            if (inserted != NULL)
                *inserted = 0;
            return 1;
        }

        index = (index + 1) & mask;
    }

    map->entries[index].key = key;
    map->entries[index].key_length = key_length;
    map->entries[index].value = value;
    map->entries[index].hash = hash;
    map->size++;

    if (inserted != NULL)
        *inserted = 1;
    return 1;
}

int luauc_string_map_find(const luauc_string_map_t* map, const char* key, size_t key_length, size_t* value)
{
    uint64_t hash;
    size_t index;
    size_t mask;

    if (map == NULL || map->capacity == 0 || (key == NULL && key_length != 0))
        return 0;

    hash = luauc_hash_bytes(key, key_length);
    mask = map->capacity - 1;
    index = (size_t)hash & mask;

    while (map->entries[index].hash != 0)
    {
        if (map->entries[index].hash == hash && map->entries[index].key_length == key_length &&
            memcmp(map->entries[index].key, key, key_length) == 0)
        {
            if (value != NULL)
                *value = map->entries[index].value;
            return 1;
        }

        index = (index + 1) & mask;
    }

    return 0;
}
