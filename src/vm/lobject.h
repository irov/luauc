// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LOBJECT_H
#define LUAUC_LOBJECT_H

#include "lua.h"
#include "lcommon.h"

/*
** Union of all collectible objects
*/
typedef union gc_object_t gc_object_t;
struct lua_page_t;

/*
** Common format_header_t for all collectible objects (in macro form, to be included in other objects)
*/
// clang-format off
#define CommonHeader \
     uint8_t tt; uint8_t marked; uint8_t memcat
// clang-format on

/*
** Common header in struct form
*/
typedef struct gc_header_t
{
    CommonHeader;
} gc_header_t;

/*
** Union of all Lua values
*/
typedef union
{
    gc_object_t* gc;
    void* p;
    double n;
    int b;
    int64_t l;
    float v[2]; // v[0], v[1] live here; v[2] lives in tvalue_t.extra
} lua_value_t;

/*
** Tagged Values
*/

typedef struct tvalue_t
{
    lua_value_t value;
    int extra[LUA_EXTRA_SIZE];
    int tt;
} tvalue_t;

// Macros to test type
#define ttisnil(o) (ttype(o) == LUA_TNIL)
#define ttisnumber(o) (ttype(o) == LUA_TNUMBER)
#define ttisinteger(o) (ttype(o) == LUA_TINTEGER)
#define ttisstring(o) (ttype(o) == LUA_TSTRING)
#define ttistable(o) (ttype(o) == LUA_TTABLE)
#define ttisfunction(o) (ttype(o) == LUA_TFUNCTION)
#define ttisboolean(o) (ttype(o) == LUA_TBOOLEAN)
#define ttisuserdata(o) (ttype(o) == LUA_TUSERDATA)
#define ttisthread(o) (ttype(o) == LUA_TTHREAD)
#define ttisbuffer(o) (ttype(o) == LUA_TBUFFER)
#define ttislightuserdata(o) (ttype(o) == LUA_TLIGHTUSERDATA)
#define ttisvector(o) (ttype(o) == LUA_TVECTOR)
#define ttisupval(o) (ttype(o) == LUA_TUPVAL)
#define ttisclass(o) (ttype(o) == LUA_TCLASS)
#define ttisobject(o) (ttype(o) == LUA_TOBJECT)

// Macros to access values
#define ttype(o) ((o)->tt)
#define gcvalue(o) check_exp(iscollectable(o), (o)->value.gc)
#define pvalue(o) check_exp(ttislightuserdata(o), (o)->value.p)
#define nvalue(o) check_exp(ttisnumber(o), (o)->value.n)
#define lvalue(o) check_exp(ttisinteger(o), (o)->value.l)
#define vvalue(o) check_exp(ttisvector(o), (o)->value.v)
#define tsvalue(o) check_exp(ttisstring(o), &(o)->value.gc->ts)
#define uvalue(o) check_exp(ttisuserdata(o), &(o)->value.gc->u)
#define clvalue(o) check_exp(ttisfunction(o), &(o)->value.gc->cl)
#define hvalue(o) check_exp(ttistable(o), &(o)->value.gc->h)
#define bvalue(o) check_exp(ttisboolean(o), (o)->value.b)
#define thvalue(o) check_exp(ttisthread(o), &(o)->value.gc->th)
#define bufvalue(o) check_exp(ttisbuffer(o), &(o)->value.gc->buf)
#define upvalue(o) check_exp(ttisupval(o), &(o)->value.gc->uv)
#define classvalue(o) check_exp(ttisclass(o), &(o)->value.gc->lclass)
#define objectvalue(o) check_exp(ttisobject(o), &(o)->value.gc->lobject)

#define l_isfalse(o) (ttisnil(o) || (ttisboolean(o) && bvalue(o) == 0))

#define lightuserdatatag(o) check_exp(ttislightuserdata(o), (o)->extra[0])

// Internal tags used by the VM
#define LU_TAG_ITERATOR LUA_UTAG_LIMIT

/*
** for internal debug only
*/
#define checkconsistency(obj) LUAU_ASSERT(!iscollectable(obj) || (ttype(obj) == (obj)->value.gc->gch.tt))

#define checkliveness(g, obj) LUAU_ASSERT(!iscollectable(obj) || ((ttype(obj) == (obj)->value.gc->gch.tt) && !isdead(g, (obj)->value.gc)))

// Macros to set values
#define setnilvalue(obj) ((obj)->tt = LUA_TNIL)

#define setnvalue(obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.n = (x); \
        i_o->tt = LUA_TNUMBER; \
    }

#define setlvalue(obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.l = (x); \
        i_o->tt = LUA_TINTEGER; \
    }

#if LUA_VECTOR_SIZE == 4
#define setvvalue(obj, x, y, z, w) \
    { \
        tvalue_t* i_o = (obj); \
        float* i_v = i_o->value.v; \
        i_v[0] = (x); \
        i_v[1] = (y); \
        i_v[2] = (z); \
        i_v[3] = (w); \
        i_o->tt = LUA_TVECTOR; \
    }
#else
#define setvvalue(obj, x, y, z, w) \
    { \
        tvalue_t* i_o = (obj); \
        float* i_v = i_o->value.v; \
        i_v[0] = (x); \
        i_v[1] = (y); \
        i_v[2] = (z); \
        i_o->tt = LUA_TVECTOR; \
    }
#endif

#define setpvalue(obj, x, tag) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.p = (x); \
        i_o->extra[0] = (tag); \
        i_o->tt = LUA_TLIGHTUSERDATA; \
    }

#define setbvalue(obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.b = (x); \
        i_o->tt = LUA_TBOOLEAN; \
    }

#define setsvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TSTRING; \
        checkliveness(L->global, i_o); \
    }

#define setuvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TUSERDATA; \
        checkliveness(L->global, i_o); \
    }

#define setthvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TTHREAD; \
        checkliveness(L->global, i_o); \
    }

#define setbufvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TBUFFER; \
        checkliveness(L->global, i_o); \
    }

#define setclvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TFUNCTION; \
        checkliveness(L->global, i_o); \
    }

#define sethvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TTABLE; \
        checkliveness(L->global, i_o); \
    }

#define setptvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TPROTO; \
        checkliveness(L->global, i_o); \
    }

#define setupvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TUPVAL; \
        checkliveness(L->global, i_o); \
    }

#define setobj(L, obj1, obj2) \
    { \
        const tvalue_t* o2 = (obj2); \
        tvalue_t* o1 = (obj1); \
        *o1 = *o2; \
        checkliveness(L->global, o1); \
    }

#define setclassvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TCLASS; \
        checkliveness(L->global, i_o); \
    }


#define setobjectvalue(L, obj, x) \
    { \
        tvalue_t* i_o = (obj); \
        i_o->value.gc = cast_to(gc_object_t*, (x)); \
        i_o->tt = LUA_TOBJECT; \
        checkliveness(L->global, i_o); \
    }

/*
** different types of sets, according to destination
*/

// to stack
#define setobj2s setobj
// from table to same table (no barrier)
#define setobjt2t setobj
// to table (needs barrier)
#define setobj2t setobj
// to new object (no barrier)
#define setobj2n setobj
// to class instance or static member (needs barrier)
#define setobj2class setobj

#define setttype(obj, tt) (ttype(obj) = (tt))

#define iscollectable(o) (ttype(o) >= LUA_TSTRING)

typedef tvalue_t* StkId; // index to stack elements

/*
** String headers for string table
*/
typedef struct tstring_t
{
    CommonHeader;
    // 1 byte padding

    int16_t atom;

    // 2 byte padding

    struct tstring_t* next; // next string in the hash table bucket

    unsigned int hash;
    unsigned int len;

    char data[1]; // string data is allocated right after the header
} tstring_t;


#define getstr(ts) (ts)->data
#define svalue(o) getstr(tsvalue(o))

typedef struct udata_t
{
    CommonHeader;

    uint8_t tag;

    int len;

    struct lua_table_t* metatable;

    // userdata is allocated right after the header
    // while the alignment is only 8 here, for sizes starting at 16 bytes, 16 byte alignment is provided
    _Alignas(8) char data[1];
} udata_t;

typedef struct luauc_vm_buffer_t
{
    CommonHeader;

    unsigned int len;

    _Alignas(8) char data[1];
} luauc_vm_buffer_t;

typedef enum feedback_vector_slot_kind_t
{
    CALL_TARGET
} feedback_vector_slot_kind_t;

typedef struct feedback_vector_slot_t
{
    feedback_vector_slot_kind_t kind;

    union
    {
        struct
        {
            uint32_t pc;
            uint32_t proto;
            uint32_t hits;
        } call_target;
    };
} feedback_vector_slot_t;

/*
** Function Prototypes
*/
// clang-format off
typedef struct proto_t
{
    CommonHeader;

    uint8_t nups; // number of upvalues
    uint8_t numparams;
    uint8_t is_vararg;
    uint8_t maxstacksize;
    uint8_t flags;

    tvalue_t* k;              // constants used by the function
    Instruction* code;      // function bytecode
    struct proto_t** p;       // functions defined inside the function
    const Instruction* codeentry;

    void* execdata;
    uintptr_t exectarget;

    uint8_t* lineinfo;      // for each instruction, line number as a delta from baseline
    int* abslineinfo;       // baseline line info, one entry for each 1<<linegaplog2 instructions; allocated after lineinfo
    struct local_var_t* locvars; // information about local variables
    tstring_t** upvalues;     // upvalue names
    tstring_t* source;

    tstring_t* debugname;
    uint8_t* debuginsn; // a copy of code[] array with just opcodes

    uint8_t* typeinfo;

    void* userdata;

    gc_object_t* gclist;

    int sizecode;
    int sizep;
    int sizelocvars;
    int sizeupvalues;
    int sizek;
    int sizelineinfo;
    int linegaplog2;
    int linedefined;
    int bytecodeid;
    int sizetypeinfo;

    feedback_vector_slot_t* feedbackvec;
    uint32_t feedbackvecsize;
    uint32_t funid;
    struct proto_t* optimized;
    struct proto_t* deoptimized;
    uint64_t cost;
} proto_t;
// clang-format on

typedef struct local_var_t
{
    tstring_t* varname;
    int startpc; // first point where variable is active
    int endpc;   // first point where variable is dead
    uint8_t reg; // register slot, relative to base, where variable is stored
} local_var_t;

/*
** Upvalues
*/

typedef struct upvalue_t
{
    CommonHeader;
    uint8_t markedopen; // set if reachable from an alive thread (only valid during atomic)

    // 4 byte padding (x64)

    tvalue_t* v; // points to stack or to its own value
    union
    {
        tvalue_t value; // the value (when closed)
        struct
        {
            // global double linked list (when open)
            struct upvalue_t* prev;
            struct upvalue_t* next;

            // thread linked list (when open)
            struct upvalue_t* threadnext;
        } open;
    } u;
} upvalue_t;

#define upisopen(up) ((up)->v != &(up)->u.value)

/*
** Closures
*/

typedef struct closure_t
{
    CommonHeader;

    uint8_t isC;
    uint8_t nupvalues;
    uint8_t stacksize;
    uint8_t preload;

    gc_object_t* gclist;
    struct lua_table_t* env;

    union
    {
        struct
        {
            lua_CFunction f;
            lua_Continuation cont;
            const char* debugname;
            tvalue_t upvals[1];
        } c;

        struct
        {
            struct proto_t* p;
            tvalue_t uprefs[1];
        } l;
    };
} closure_t;

#define iscfunction(o) (ttype(o) == LUA_TFUNCTION && clvalue(o)->isC)
#define isLfunction(o) (ttype(o) == LUA_TFUNCTION && !clvalue(o)->isC)

/*
** Tables
*/

typedef struct table_key_t
{
    lua_value_t value;
    int extra[LUA_EXTRA_SIZE];
    unsigned tt : 4;
    int next : 28; // for chaining
} table_key_t;

typedef struct lua_node_t
{
    tvalue_t val;
    table_key_t key;
} lua_node_t;

// copy a value into a key
#define setnodekey(L, node, obj) \
    { \
        lua_node_t* n_ = (node); \
        const tvalue_t* i_o = (obj); \
        n_->key.value = i_o->value; \
        memcpy(n_->key.extra, i_o->extra, sizeof(n_->key.extra)); \
        n_->key.tt = i_o->tt; \
        checkliveness(L->global, i_o); \
    }

// copy a value from a key
#define getnodekey(L, obj, node) \
    { \
        tvalue_t* i_o = (obj); \
        const lua_node_t* n_ = (node); \
        i_o->value = n_->key.value; \
        memcpy(i_o->extra, n_->key.extra, sizeof(i_o->extra)); \
        i_o->tt = n_->key.tt; \
        checkliveness(L->global, i_o); \
    }

// clang-format off
typedef struct lua_table_t
{
    CommonHeader;

    uint8_t tmcache;    // 1<<p means tagmethod(p) is not present
    uint8_t readonly;   // sandboxing feature to prohibit writes to table
    uint8_t safeenv;    // environment doesn't share globals with other scripts
    uint8_t lsizenode;  // log2 of size of `node' array
    uint8_t nodemask8;  // (1<<lsizenode)-1, truncated to 8 bits

    int sizearray; // size of `array' array
    union
    {
        int lastfree;  // any free position is before this position
        int aboundary; // negated 'boundary' of `array' array; iff aboundary < 0
    };

    struct lua_table_t* metatable;
    tvalue_t* array;  // array part
    lua_node_t* node;
    gc_object_t* gclist;
} lua_table_t;
// clang-format on

typedef struct luauc_class_t
{
    CommonHeader;

    gc_object_t* gclist;

    tstring_t* name;

    // Mapping from offset to static members (only methods for now).
    tvalue_t* staticmembers;

    // Mapping from member name to offset.
    lua_table_t* memberstooffset;

    // Mapping from offset to member name.
    tstring_t** offsettomember;

    // Metatable for this *class object*. At time of writing this only contains
    // __call, but we may add more metamethods to class objects in the future.
    lua_table_t* metatable;

    // Metatable for instances of this class. NULL until the first metamethod
    // is added via luaR_addclassmember.
    lua_table_t* instancemetatable;

    // Number of instance members that we expect instances of this class object
    // to have.
    int numberofinstancemembers;

    // Total number of members that we expect this class object to have between
    // instance and static members.
    //
    // We store this number as an optimization. It's pretty rare that we need
    // to reference the specific number of static members, but it's very common
    // to reference the total number of members (for validating hot paths in
    // the interpreter) and the number of instance members (branching on
    // instance or static members, creating class instances).
    int numberofallmembers;

} luauc_class_t;

typedef struct luauc_object_t
{
    CommonHeader;

    gc_object_t* gclist;

    // The class object that this value is an instance of.
    luauc_class_t* lclass;

    // The number of members that this instance contains. We need this in order
    // to free ourselves if we got swept in the same GC cycle as our class
    // pointer.
    int numberofmembers;

    // The fields of this instance.
    tvalue_t* members;

} luauc_object_t;

/*
** `module' operation for hashing (size is always a power of 2)
*/
static inline int __lmod(unsigned int value, int size)
{
    LUAU_ASSERT((size & (size - 1)) == 0);
    return (int)(value & (unsigned int)(size - 1));
}

#define lmod(s, size) __lmod((unsigned int)(s), (int)(size))

#define twoto(x) ((int)(1 << (x)))
#define sizenode(t) (twoto((t)->lsizenode))

#define luaO_nilobject (&luaO_nilobject_)

LUAI_DATA const tvalue_t luaO_nilobject_;

#define ceillog2(x) (luaO_log2((x)-1) + 1)

LUAI_FUNC int luaO_log2(unsigned int x);
LUAI_FUNC int luaO_rawequalObj(const tvalue_t* t1, const tvalue_t* t2);
LUAI_FUNC int luaO_rawequalKey(const table_key_t* t1, const tvalue_t* t2);
LUAI_FUNC int luaO_str2d(const char* s, double* result);
LUAI_FUNC int luaO_str2l(const char* s, int64_t* result, int base);
LUAI_FUNC const char* luaO_pushvfstring(lua_State* L, const char* fmt, va_list argp);
LUAI_FUNC const char* luaO_pushfstring(lua_State* L, const char* fmt, ...);
LUAI_FUNC const char* luaO_chunkid(char* buf, size_t buflen, const char* source, size_t srclen);

#endif
