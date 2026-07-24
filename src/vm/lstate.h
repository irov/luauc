// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LSTATE_H
#define LUAUC_LSTATE_H

#include "lobject.h"
#include "ltm.h"
#include "ludata.h"

// registry
#define registry(L) (&L->global->registry)

// extra stack space to handle TM calls and some other extras
#define EXTRA_STACK 5

#define BASIC_CI_SIZE 8

#define BASIC_STACK_SIZE (2 * LUA_MINSTACK)

// clang-format off
typedef struct string_table_t
{
    tstring_t** hash;
    uint32_t nuse; // number of elements
    int size;
} string_table_t;
// clang-format on

/*
** informations about a call
**
** the general Lua stack frame structure is as follows:
** - each function gets a stack frame, with function "registers" being stack slots on the frame
** - function arguments are associated with registers 0+
** - function locals and temporaries follow after; usually locals are a consecutive block per scope, and temporaries are allocated after this, but
*this is up to the compiler
**
** when function doesn't have varargs, the stack layout is as follows:
** ^ (func) ^^ [fixed args] [locals + temporaries]
** where ^ is the 'func' pointer in call_info_t struct, and ^^ is the 'base' pointer (which is what registers are relative to)
**
** when function *does* have varargs, the stack layout is more complex - the runtime has to copy the fixed arguments so that the 0+ addressing still
*works as follows:
** ^ (func) [fixed args] [varargs] ^^ [fixed args] [locals + temporaries]
**
** computing the sizes of these individual blocks works as follows:
** - the number of fixed args is always matching the `numparams` in a function's proto_t object; runtime adds `nil` during the call execution as
*necessary
** - the number of variadic args can be computed by evaluating (ci->base - ci->func - 1 - numparams)
**
** the call_info_t structures are allocated as an array, with each subsequent call being *appended* to this array (so if f calls g, call_info_t for g
*immediately follows call_info_t for f)
** the `nresults` field in call_info_t is set by the caller to tell the function how many arguments the caller is expecting on the stack after the
*function returns
** the `flags` field in call_info_t contains internal execution flags that are important for pcall/etc, see LUA_CALLINFO_*
*/
// clang-format off
typedef struct call_info_t
{
    StkId base;    // base for this function
    StkId func;    // function index in the stack
    StkId top;     // top for this function
    proto_t* p;

    union
    {
        const Instruction* savedpc;
        int errfunc; // For C functions, the error function index in the stack
    };

    int nresults;       // expected number of results from this function
    unsigned int flags; // call frame flags, see LUA_CALLINFO_*
} call_info_t;
// clang-format on

#define LUA_CALLINFO_RETURN (1 << 0) // should the interpreter return after returning from this callinfo? first frame must have this set
#define LUA_CALLINFO_HANDLE (1 << 1) // should the error thrown during execution get handled by continuation from this callinfo? func must be C
#define LUA_CALLINFO_NATIVE (1 << 2) // should this function be executed using execution callback for native code
#define LUA_CALLINFO_OPYIELD (1 << 3) // call frame has yielded on a non-call opcode and requires luaV_finishop

#define curr_func(L) (clvalue(L->ci->func))
#define ci_func(ci) (clvalue((ci)->func))
#define f_isLua(ci) (!ci_func(ci)->isC)
#define isLua(ci) (ttisfunction((ci)->func) && f_isLua(ci))

typedef struct gc_stats_t
{
    // data for proportional-integral controller of heap trigger value
    int32_t triggerterms[32];
    uint32_t triggertermpos;
    int32_t triggerintegral;

    size_t atomicstarttotalsizebytes;
    size_t endtotalsizebytes;
    size_t heapgoalsizebytes;

    double starttimestamp;
    double atomicstarttimestamp;
    double endtimestamp;
} gc_stats_t;

#ifdef LUAI_GCMETRICS
typedef struct gc_cycle_metrics_t
{
    size_t starttotalsizebytes;
    size_t heaptriggersizebytes;

    double pausetime; // time from end of the last cycle to the start of a new one

    double starttimestamp;
    double endtimestamp;

    double marktime;
    double markassisttime;
    double markmaxexplicittime;
    size_t markexplicitsteps;
    size_t markwork;

    double atomicstarttimestamp;
    size_t atomicstarttotalsizebytes;
    double atomictime;

    // specific atomic stage parts
    double atomictimeupval;
    double atomictimeweak;
    double atomictimegray;
    double atomictimeclear;

    double sweeptime;
    double sweepassisttime;
    double sweepmaxexplicittime;
    size_t sweepexplicitsteps;
    size_t sweepwork;

    size_t assistwork;
    size_t explicitwork;

    size_t propagatework;
    size_t propagateagainwork;

    size_t endtotalsizebytes;
} gc_cycle_metrics_t;

typedef struct gc_metrics_t
{
    double stepexplicittimeacc;
    double stepassisttimeacc;

    // when cycle is completed, last cycle values are updated
    uint64_t completedcycles;

    gc_cycle_metrics_t lastcycle;
    gc_cycle_metrics_t currcycle;
} gc_metrics_t;
#endif

// Callbacks that can be used to to redirect code execution from Luau bytecode VM to a custom implementation (AoT/JiT/sandboxing/...)
typedef struct lua_execution_callbacks_t
{
    void* context;
    void (*close)(lua_State* L);                 // called when global VM state is closed
    void (*destroy)(lua_State* L, proto_t* proto); // called when function is destroyed
    int (*enter)(lua_State* L, proto_t* proto);    // called when function is about to start/resume (when execdata is present), return 0 to exit VM
    void (*disable)(lua_State* L, proto_t* proto); // called when function has to be switched from native to bytecode in the debugger
    size_t (*getmemorysize)(lua_State* L, proto_t* proto); // called to request the size of memory associated with native part of the proto_t
    uint8_t (*gettypemapping)(lua_State* L, const char* str, size_t len); // called to get the userdata type index
    char* (*getcounterdata)(
        lua_State* L,
        proto_t* proto,
        size_t* count
    ); // called to get the execution counter data and count {uint32_t, uint32_t, uint64_t}
    proto_t* (*inlinefunction)(lua_State* L, closure_t* caller, closure_t* target, uint32_t pc); // called when inlining threshold is reached
} lua_execution_callbacks_t;

typedef struct lua_udata_direct_access_data_t
{
    tvalue_t indextm;
    tvalue_t newindextm;
    tvalue_t namecalltm;
    lua_UserdataDirectAccess index;
    lua_UserdataDirectAccess newindex;
    lua_UserdataDirectNamecall namecall;
} lua_udata_direct_access_data_t;

/*
** `global state', shared by all threads of this state
*/
// clang-format off
typedef struct global_state_t
{
    string_table_t strt; // hash table for strings

    lua_Alloc frealloc;   // function to reallocate memory
    void* ud;             // auxiliary data to `frealloc'

    uint8_t currentwhite;
    uint8_t gcstate; // state of garbage collector

    gc_object_t* gray;      // list of gray objects
    gc_object_t* grayagain; // list of objects to be traversed atomically
    gc_object_t* weak;      // list of weak tables (to be cleared)

    size_t GCthreshold;                       // when totalbytes >= GCthreshold, run GC step
    size_t totalbytes;                        // number of bytes currently allocated

    int gcgoal;                               // see LUAI_GCGOAL
    int gcstepmul;                            // see LUAI_GCSTEPMUL
    int gcstepsize;                           // see LUAI_GCSTEPSIZE

    struct lua_page_t* freepages[LUA_SIZECLASSES]; // free page linked list for each size class for non-collectable objects
    struct lua_page_t* freegcopages[LUA_SIZECLASSES]; // free page linked list for each size class for collectable objects
    struct lua_page_t* allpages; // page linked list with all pages for all non-collectable object classes (available with LUAU_ASSERTENABLED)
    struct lua_page_t* allgcopages; // page linked list with all pages for all collectable object classes
    struct lua_page_t* sweepgcopage; // position of the sweep in `allgcopages'

    struct lua_State* mainthread;
    upvalue_t uvhead; // head of double-linked list of all open upvalues
    struct lua_table_t* mt[LUA_T_COUNT]; // metatables for basic types
    tstring_t* ttname[LUA_T_COUNT]; // names for basic types
    tstring_t* tmname[TM_N]; // array with tag-method names

    tvalue_t pseudotemp; // storage for temporary values used in pseudo2addr

    tvalue_t registry; // registry table, used by lua_ref and LUA_REGISTRYINDEX
    int registryfree; // next free slot in registry

    struct lua_jmpbuf_t* errorjmp; // jump buffer data for longjmp-style error handling

    uint64_t rngstate; // PCG random number generator state
    uint64_t ptrenckey[4]; // pointer encoding key for display

    lua_Callbacks cb;

    lua_execution_callbacks_t ecb;

    _Alignas(16) uint8_t ecbdata[LUA_EXECUTION_CALLBACK_STORAGE];

    // Set of userdata __index/__newindex/__namecall metamethods for a direct access
    lua_udata_direct_access_data_t udatadirect[UTAG_INTERNAL_LIMIT];

    size_t memcatbytes[LUA_MEMORY_CATEGORIES]; // total amount of memory used by each memory category

    void (*udatagc[LUA_UTAG_LIMIT])(lua_State*, void*); // for each userdata tag, a gc callback to be called immediately before freeing memory
    lua_table_t* udatamt[LUA_UTAG_LIMIT]; // metatables for tagged userdata

    tstring_t* lightuserdataname[LUA_LUTAG_LIMIT]; // names for tagged lightuserdata

    // per-tag direct field dispatch tables; NULL until first field is registered for that tag
    struct lua_table_t* udatadirectfields[UTAG_INTERNAL_LIMIT];

    gc_stats_t gcstats;
    uint32_t lastprotoid;

#ifdef LUAI_GCMETRICS
    gc_metrics_t gcmetrics;
#endif
} global_state_t;
// clang-format on

/*
** `per thread' state
*/
// clang-format off
struct lua_State
{
    CommonHeader;
    uint8_t status;

    uint8_t activememcat; // memory category that is used for new GC object allocations

    bool isactive;   // thread is currently executing, stack may be mutated without barriers
    bool singlestep; // call debugstep hook after each instruction

    StkId top;                                        // first free slot in the stack
    StkId base;                                       // base of current function
    global_state_t* global;
    call_info_t* ci;                                     // call info for current function
    StkId stack_last;                                 // last free slot in the stack
    StkId stack;                                      // stack base

    call_info_t* end_ci;                          // points after end of ci array
    call_info_t* base_ci;                         // array of call_info_t's

    int stacksize;
    int size_ci;                               // size of array `base_ci'

    unsigned short nCcalls;     // number of nested C calls
    unsigned short baseCcalls;  // nested C calls when resuming coroutine

    int cachedslot;    // when table operations or INDEX/NEWINDEX is invoked from Luau, what is the expected slot for lookup?

    lua_table_t* gt;           // table of globals
    upvalue_t* openupval;       // list of open upvalues in this stack
    gc_object_t* gclist;

    tstring_t* namecall; // when invoked from Luau using NAMECALL, what method do we need to invoke?

    void* userdata;
};
// clang-format on

/*
** Union of all collectible objects
*/
union gc_object_t
{
    gc_header_t gch;
    struct tstring_t ts;
    struct udata_t u;
    struct closure_t cl;
    struct lua_table_t h;
    struct proto_t p;
    struct upvalue_t uv;
    struct lua_State th; // thread
    struct luauc_vm_buffer_t buf;
    struct luauc_class_t lclass;
    struct luauc_object_t lobject;
};

// macros to convert a gc_object_t into a specific value
#define gco2ts(o) check_exp((o)->gch.tt == LUA_TSTRING, &((o)->ts))
#define gco2u(o) check_exp((o)->gch.tt == LUA_TUSERDATA, &((o)->u))
#define gco2cl(o) check_exp((o)->gch.tt == LUA_TFUNCTION, &((o)->cl))
#define gco2h(o) check_exp((o)->gch.tt == LUA_TTABLE, &((o)->h))
#define gco2p(o) check_exp((o)->gch.tt == LUA_TPROTO, &((o)->p))
#define gco2uv(o) check_exp((o)->gch.tt == LUA_TUPVAL, &((o)->uv))
#define gco2th(o) check_exp((o)->gch.tt == LUA_TTHREAD, &((o)->th))
#define gco2buf(o) check_exp((o)->gch.tt == LUA_TBUFFER, &((o)->buf))
#define gco2class(o) check_exp((o)->gch.tt == LUA_TCLASS, &((o)->lclass))
#define gco2object(o) check_exp((o)->gch.tt == LUA_TOBJECT, &((o)->lobject))

// macro to convert any Lua object into a gc_object_t
#define obj2gco(v) check_exp(iscollectable(v), cast_to(gc_object_t*, (v) + 0))

LUAI_FUNC lua_State* luaE_newthread(lua_State* L);
LUAI_FUNC void luaE_freethread(lua_State* L, lua_State* L1, struct lua_page_t* page);

#endif
