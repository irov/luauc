// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#ifndef LUAUC_LBUFFER_H
#define LUAUC_LBUFFER_H

#include "lobject.h"

// buffer size limit
#define MAX_BUFFER_SIZE (1 << 30)

// gc_object_t size has to be at least 16 bytes, so a minimum of 8 bytes is always reserved
#define sizebuffer(len) (offsetof(luauc_vm_buffer_t, data) + ((len) < 8 ? 8 : (len)))

LUAI_FUNC luauc_vm_buffer_t* luaB_newbuffer(lua_State* L, size_t s);
LUAI_FUNC void luaB_freebuffer(lua_State* L, luauc_vm_buffer_t* u, struct lua_page_t* page);

#endif
