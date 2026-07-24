// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#include "common/luauc_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct failing_allocator_t
{
    size_t calls;
    size_t fail_after;
} failing_allocator_t;

static void* __fail_reallocate(void* context, void* pointer, size_t old_size, size_t new_size)
{
    failing_allocator_t* state = (failing_allocator_t*)context;
    (void)old_size;

    if (new_size == 0)
    {
        free(pointer);
        return NULL;
    }

    if (state->calls++ >= state->fail_after)
        return NULL;

    return realloc(pointer, new_size);
}

static int __test_vector(void)
{
    luauc_vector_t vector;
    int value;
    size_t index;

    if (!luauc_vector_init(&vector, sizeof(int), luauc_default_allocator()))
        return 0;

    for (index = 0; index < 1000; ++index)
    {
        value = (int)(index * 3);
        if (luauc_vector_push(&vector, &value) == NULL)
            return 0;
    }

    for (index = 0; index < vector.size; ++index)
    {
        const int* stored = (const int*)luauc_vector_at_const(&vector, index);
        if (stored == NULL || *stored != (int)(index * 3))
            return 0;
    }

    if (luauc_vector_at(&vector, vector.size) != NULL)
        return 0;

    luauc_vector_destroy(&vector);
    return 1;
}

static int __test_buffer(void)
{
    static const unsigned char __expected[] = {0x11, 0x44, 0x33, 0x22, 0x11, 0xac, 0x02};
    luauc_buffer_t buffer;
    unsigned char* bytes;
    size_t size;

    luauc_buffer_init(&buffer, luauc_default_allocator());
    if (!luauc_buffer_append_byte(&buffer, 0x11) ||
        !luauc_buffer_append_u32(&buffer, UINT32_C(0x11223344)) ||
        !luauc_buffer_append_varuint(&buffer, 300))
        return 0;

    bytes = luauc_buffer_release(&buffer, &size);
    if (size != sizeof(__expected) || memcmp(bytes, __expected, size) != 0)
        return 0;

    free(bytes);
    luauc_buffer_destroy(&buffer);
    return 1;
}

static int __test_arena_and_map(void)
{
    luauc_arena_t arena;
    luauc_string_map_t map;
    size_t index;

    luauc_arena_init(&arena, 128, luauc_default_allocator());
    luauc_string_map_init(&map, luauc_default_allocator());

    for (index = 0; index < 100; ++index)
    {
        char temporary[32];
        int length = snprintf(temporary, sizeof(temporary), "name_%zu", index);
        char* key;
        int inserted;

        if (length <= 0)
            return 0;

        key = luauc_arena_duplicate(&arena, temporary, (size_t)length);
        if (key == NULL || !luauc_string_map_insert(&map, key, (size_t)length, index * 7, &inserted) || !inserted)
            return 0;
    }

    for (index = 0; index < 100; ++index)
    {
        char temporary[32];
        int length = snprintf(temporary, sizeof(temporary), "name_%zu", index);
        size_t value = 0;

        if (length <= 0 || !luauc_string_map_find(&map, temporary, (size_t)length, &value) || value != index * 7)
            return 0;
    }

    luauc_string_map_destroy(&map);
    luauc_arena_destroy(&arena);
    return 1;
}

static int __test_overflow_and_oom(void)
{
    failing_allocator_t state;
    luauc_allocator_t allocator;
    luauc_vector_t vector;
    size_t result;
    int value = 1;

    if (luauc_size_add(SIZE_MAX, 1, &result) || luauc_size_multiply(SIZE_MAX, 2, &result))
        return 0;

    state.calls = 0;
    state.fail_after = 0;
    allocator.context = &state;
    allocator.reallocate = __fail_reallocate;

    if (!luauc_vector_init(&vector, sizeof(int), allocator))
        return 0;
    if (luauc_vector_push(&vector, &value) != NULL)
        return 0;

    luauc_vector_destroy(&vector);
    return 1;
}

int main(void)
{
    if (!__test_vector())
        return 1;
    if (!__test_buffer())
        return 2;
    if (!__test_arena_and_map())
        return 3;
    if (!__test_overflow_and_oom())
        return 4;

    return 0;
}
