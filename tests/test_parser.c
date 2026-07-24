// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "ast/luauc_parser.h"

#include <stdio.h>
#include <stdlib.h>

static char* __read_file(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    char* data;
    long length;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
        return NULL;
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return NULL;
    }
    data = (char*)malloc((size_t)length);
    if (data != NULL && fread(data, 1, (size_t)length, file) != (size_t)length)
    {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

int main(int argc, char** argv)
{
    luauc_allocator_t allocator = luauc_default_allocator();
    luauc_arena_t arena;
    luauc_name_table_t names;
    luauc_parse_result_t result;
    char* source;
    size_t size;
    int status = 1;

    if (argc != 2)
        return 2;
    source = __read_file(argv[1], &size);
    if (source == NULL)
        return 2;
    luauc_arena_init(&arena, 16384, allocator);
    if (!luauc_name_table_init(&names, &arena, allocator))
        goto cleanup_arena;
    if (!luauc_parse(source, size, &arena, &names, allocator, &result))
        goto cleanup_names;
    if (result.has_error)
    {
        fprintf(
            stderr,
            "%s:%u:%u: %s\n",
            argv[1],
            result.error_location.begin.line + 1,
            result.error_location.begin.column + 1,
            result.error_message
        );
        goto cleanup_names;
    }
    status = 0;

cleanup_names:
    luauc_name_table_destroy(&names);
cleanup_arena:
    luauc_arena_destroy(&arena);
    free(source);
    return status;
}
