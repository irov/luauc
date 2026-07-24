// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "bytecode/luauc_bytecode_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char* __read_file(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    unsigned char* data;
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
        return NULL;
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return NULL;
    }
    data = (unsigned char*)malloc((size_t)length);
    if (data == NULL)
    {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length)
    {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

int main(int argc, char** argv)
{
    luauc_bytecode_builder_t builder;
    unsigned char* actual;
    unsigned char* expected;
    size_t actual_size;
    size_t expected_size;
    uint32_t function_id;
    int result = 1;

    if (argc != 2 || !luauc_bytecode_builder_init(&builder, luauc_default_allocator()))
        return 2;

    function_id = luauc_bytecode_begin_function(&builder, 0, 1);
    luauc_bytecode_set_debug_function_line_defined(&builder, 1);
    luauc_bytecode_set_debug_line(&builder, 1);
    luauc_bytecode_emit_abc(&builder, LOP_PREPVARARGS, 0, 0, 0);
    luauc_bytecode_emit_ad(&builder, LOP_LOADN, 0, 1);
    luauc_bytecode_emit_abc(&builder, LOP_RETURN, 0, 2, 0);

    if (!luauc_bytecode_end_function(&builder, 1, 0, LPF_NATIVE_COLD, 0))
        goto cleanup;
    luauc_bytecode_set_main_function(&builder, function_id);
    if (!luauc_bytecode_finalize(&builder))
        goto cleanup;

    actual = luauc_bytecode_release(&builder, &actual_size);
    expected = __read_file(argv[1], &expected_size);
    if (actual == NULL || expected == NULL)
    {
        free(actual);
        free(expected);
        goto cleanup;
    }

    if (actual_size != expected_size || memcmp(actual, expected, actual_size) != 0)
    {
        fprintf(stderr, "bytecode mismatch: C=%zu bytes, oracle=%zu bytes\n", actual_size, expected_size);
        free(actual);
        free(expected);
        goto cleanup;
    }

    free(actual);
    free(expected);
    result = 0;

cleanup:
    luauc_bytecode_builder_destroy(&builder);
    return result;
}
