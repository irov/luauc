// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LUACONF_H
#define LUAUC_LUACONF_H

// When debugging complex issues, consider enabling one of these:
// This will reallocate the stack very aggressively at every opportunity; use this with asan to catch stale stack pointers
// #define HARDSTACKTESTS 1
// This will call GC validation very aggressively at every incremental GC step; use this with caution as it's SLOW
// #define HARDMEMTESTS 1
// This will call GC validation very aggressively at every GC opportunity; use this with caution as it's VERY SLOW
// #define HARDMEMTESTS 2

#define LUAU_FASTMATH_BEGIN
#define LUAU_FASTMATH_END

#define LUA_PRINTF_ATTR(fmt, arg)

#define LUA_NORETURN _Noreturn

// Can be used to reconfigure visibility/exports for public APIs
#ifndef LUA_API
#define LUA_API extern
#endif

#define LUALIB_API LUA_API

// Can be used to reconfigure visibility for internal APIs
#define LUAI_FUNC extern
#define LUAI_DATA extern

// Can be used to reconfigure internal error handling to use longjmp instead of C++ EH
#ifndef LUA_USE_LONGJMP
#define LUA_USE_LONGJMP 0
#endif

// LUA_IDSIZE gives the maximum size for the description of the source
#ifndef LUA_IDSIZE
#define LUA_IDSIZE 256
#endif

// LUA_MINSTACK is the initial number of reserved stack slots for a C function
#ifndef LUA_MINSTACK
#define LUA_MINSTACK 20
#endif

// LUAI_MAXCSTACK limits the number of Lua stack slots that a C function can use
#ifndef LUAI_MAXCSTACK
#define LUAI_MAXCSTACK 8000
#endif

// LUAI_MAXCALLS limits the number of nested calls
#ifndef LUAI_MAXCALLS
#define LUAI_MAXCALLS 20000
#endif

// LUAI_MAXCCALLS is the maximum depth for nested C calls; this limit depends on native stack size
#ifndef LUAI_MAXCCALLS
#define LUAI_MAXCCALLS 200
#endif

// buffer size used for on-stack string operations; this limit depends on native stack size
#ifndef LUA_BUFFERSIZE
#define LUA_BUFFERSIZE 512
#endif

// number of valid Lua userdata tags
#ifndef LUA_UTAG_LIMIT
#define LUA_UTAG_LIMIT 128
#endif

// number of valid Lua lightuserdata tags
#ifndef LUA_LUTAG_LIMIT
#define LUA_LUTAG_LIMIT 128
#endif

// upper bound for number of size classes used by page allocator
#ifndef LUA_SIZECLASSES
#define LUA_SIZECLASSES 40
#endif

// available number of separate memory categories
#ifndef LUA_MEMORY_CATEGORIES
#define LUA_MEMORY_CATEGORIES 256
#endif

// extra storage for execution callbacks in global state
#ifndef LUA_EXECUTION_CALLBACK_STORAGE
#define LUA_EXECUTION_CALLBACK_STORAGE 512
#endif

// minimum size for the string table (must be power of 2)
#ifndef LUA_MINSTRTABSIZE
#define LUA_MINSTRTABSIZE 32
#endif

// maximum number of captures supported by pattern matching
#ifndef LUA_MAXCAPTURES
#define LUA_MAXCAPTURES 32
#endif

// }==================================================================

#ifndef LUA_VECTOR_SIZE
#define LUA_VECTOR_SIZE 3 // must be 3 or 4
#endif

#define LUA_EXTRA_SIZE (LUA_VECTOR_SIZE - 2)

#endif
