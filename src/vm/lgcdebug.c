// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lgc.h"

#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ludata.h"
#include "lbuffer.h"

#include <string.h>
#include <stdio.h>

LUAU_FASTFLAG(LuauCIProto)

static void __validateobjref(global_state_t* g, gc_object_t* f, gc_object_t* t)
{
    LUAU_ASSERT(!isdead(g, t));

    if (keepinvariant(g))
    {
        // basic incremental invariant: black can't point to white
        LUAU_ASSERT(!(isblack(f) && iswhite(t)));
    }
}

static void __validateref(global_state_t* g, gc_object_t* f, tvalue_t* v)
{
    if (iscollectable(v))
    {
        LUAU_ASSERT(ttype(v) == gcvalue(v)->gch.tt);
        __validateobjref(g, f, gcvalue(v));
    }
}

static void __validatetable(global_state_t* g, lua_table_t* h)
{
    int sizenode = 1 << h->lsizenode;

    LUAU_ASSERT(h->lastfree <= sizenode);

    if (h->metatable)
        __validateobjref(g, obj2gco(h), obj2gco(h->metatable));

    for (int i = 0; i < h->sizearray; ++i)
        __validateref(g, obj2gco(h), &h->array[i]);

    for (int i = 0; i < sizenode; ++i)
    {
        lua_node_t* n = &h->node[i];

        LUAU_ASSERT(ttype(gkey(n)) != LUA_TDEADKEY || ttisnil(gval(n)));
        LUAU_ASSERT(i + gnext(n) >= 0 && i + gnext(n) < sizenode);

        if (!ttisnil(gval(n)))
        {
            tvalue_t k = {0};
            k.tt = gkey(n)->tt;
            k.value = gkey(n)->value;

            __validateref(g, obj2gco(h), &k);
            __validateref(g, obj2gco(h), gval(n));
        }
    }
}

static void __validateclosure(global_state_t* g, closure_t* cl)
{
    __validateobjref(g, obj2gco(cl), obj2gco(cl->env));

    if (cl->isC)
    {
        for (int i = 0; i < cl->nupvalues; ++i)
            __validateref(g, obj2gco(cl), &cl->c.upvals[i]);
    }
    else
    {
        LUAU_ASSERT(cl->nupvalues == cl->l.p->nups);

        __validateobjref(g, obj2gco(cl), obj2gco(cl->l.p));

        for (int i = 0; i < cl->nupvalues; ++i)
            __validateref(g, obj2gco(cl), &cl->l.uprefs[i]);
    }
}

static void __validatestack(global_state_t* g, lua_State* l)
{
    __validateobjref(g, obj2gco(l), obj2gco(l->gt));

    for (call_info_t* ci = l->base_ci; ci <= l->ci; ++ci)
    {
        LUAU_ASSERT(l->stack <= ci->base);
        LUAU_ASSERT(ci->func <= ci->base && ci->base <= ci->top);
        LUAU_ASSERT(ci->top <= l->stack_last);
    }

    // note: stack refs can violate gc invariant so we only check for liveness
    for (StkId o = l->stack; o < l->top; ++o)
        checkliveness(g, o);

    if (l->namecall)
        __validateobjref(g, obj2gco(l), obj2gco(l->namecall));

    for (upvalue_t* uv = l->openupval; uv; uv = uv->u.open.threadnext)
    {
        LUAU_ASSERT(uv->tt == LUA_TUPVAL);
        LUAU_ASSERT(upisopen(uv));
        LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);
        LUAU_ASSERT(!isblack(obj2gco(uv))); // open upvalues are never black
    }
}

static void __validateproto(global_state_t* g, proto_t* f)
{
    if (f->source)
        __validateobjref(g, obj2gco(f), obj2gco(f->source));

    if (f->debugname)
        __validateobjref(g, obj2gco(f), obj2gco(f->debugname));

    for (int i = 0; i < f->sizek; ++i)
        __validateref(g, obj2gco(f), &f->k[i]);

    for (int i = 0; i < f->sizeupvalues; ++i)
        if (f->upvalues[i])
            __validateobjref(g, obj2gco(f), obj2gco(f->upvalues[i]));

    for (int i = 0; i < f->sizep; ++i)
        if (f->p[i])
            __validateobjref(g, obj2gco(f), obj2gco(f->p[i]));

    for (int i = 0; i < f->sizelocvars; i++)
        if (f->locvars[i].varname)
            __validateobjref(g, obj2gco(f), obj2gco(f->locvars[i].varname));
}

static void __validateclass(global_state_t* g, luauc_class_t* lco)
{
    gc_object_t* obj = obj2gco(lco);
    __validateobjref(g, obj, obj2gco(lco->name));
    __validateobjref(g, obj, obj2gco(lco->memberstooffset));
    for (int i = 0; i < lco->numberofallmembers; i++)
    {
        __validateobjref(g, obj, obj2gco(lco->offsettomember[i]));
        if (i >= lco->numberofinstancemembers)
            __validateref(g, obj, &lco->staticmembers[i - lco->numberofinstancemembers]);
    }
    __validateobjref(g, obj, obj2gco(lco->metatable));
    if (lco->instancemetatable)
        __validateobjref(g, obj, obj2gco(lco->instancemetatable));
}

static void __validateobject(global_state_t* g, luauc_object_t* inst)
{
    gc_object_t* obj = obj2gco(inst);
    __validateobjref(g, obj, obj2gco(inst->lclass));
    for (int i = 0; i < inst->numberofmembers; i++)
        __validateref(g, obj, &inst->members[i]);
}

static void __validateobj(global_state_t* g, gc_object_t* o)
{
    // dead objects can only occur during sweep
    if (isdead(g, o))
    {
        LUAU_ASSERT(g->gcstate == GCSsweep);
        return;
    }

    switch (o->gch.tt)
    {
    case LUA_TSTRING:
        break;

    case LUA_TTABLE:
        __validatetable(g, gco2h(o));
        break;

    case LUA_TFUNCTION:
        __validateclosure(g, gco2cl(o));
        break;

    case LUA_TUSERDATA:
        if (gco2u(o)->metatable)
            __validateobjref(g, o, obj2gco(gco2u(o)->metatable));
        break;

    case LUA_TTHREAD:
        __validatestack(g, gco2th(o));
        break;

    case LUA_TBUFFER:
        break;

    case LUA_TPROTO:
        __validateproto(g, gco2p(o));
        break;

    case LUA_TUPVAL:
        __validateref(g, o, gco2uv(o)->v);
        break;

    case LUA_TCLASS:
        __validateclass(g, gco2class(o));
        break;

    case LUA_TOBJECT:
        __validateobject(g, gco2object(o));
        break;

    default:
        LUAU_ASSERT(!"unexpected object type");
    }
}

static void __validategraylist(global_state_t* g, gc_object_t* o)
{
    if (!keepinvariant(g))
        return;

    while (o)
    {
        LUAU_ASSERT(isgray(o));

        switch (o->gch.tt)
        {
        case LUA_TTABLE:
            o = gco2h(o)->gclist;
            break;
        case LUA_TFUNCTION:
            o = gco2cl(o)->gclist;
            break;
        case LUA_TTHREAD:
            o = gco2th(o)->gclist;
            break;
        case LUA_TCLASS:
            o = gco2class(o)->gclist;
            break;
        case LUA_TOBJECT:
            o = gco2object(o)->gclist;
            break;
        case LUA_TPROTO:
            o = gco2p(o)->gclist;
            break;
        default:
            LUAU_ASSERT(!"unknown object in gray list");
            return;
        }
    }
}

static bool __validategco(void* context, lua_page_t* page, gc_object_t* gco)
{
    (void)page;
    lua_State* L = (lua_State*)context;
    global_state_t* g = L->global;

    __validateobj(g, gco);
    return false;
}

void luaC_validate(lua_State* L)
{
    global_state_t* g = L->global;

    LUAU_ASSERT(!isdead(g, obj2gco(g->mainthread)));
    checkliveness(g, &g->registry);

    for (int i = 0; i < LUA_T_COUNT; ++i)
    {
        if (g->mt[i])
            LUAU_ASSERT(!isdead(g, obj2gco(g->mt[i])));
    }

    for (int i = 0; i < LUA_UTAG_LIMIT; i++)
    {
        if (g->udatamt[i])
            LUAU_ASSERT(!isdead(g, obj2gco(g->udatamt[i])));
    }

    __validategraylist(g, g->weak);
    __validategraylist(g, g->gray);
    __validategraylist(g, g->grayagain);

    __validategco(L, NULL, obj2gco(g->mainthread));

    luaM_visitgco(L, L, __validategco);

    for (upvalue_t* uv = g->uvhead.u.open.next; uv != &g->uvhead; uv = uv->u.open.next)
    {
        LUAU_ASSERT(uv->tt == LUA_TUPVAL);
        LUAU_ASSERT(upisopen(uv));
        LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);
        LUAU_ASSERT(!isblack(obj2gco(uv))); // open upvalues are never black
    }
}

static inline bool __safejson(char ch)
{
    return ((unsigned)(ch)) < 128 && ch >= 32 && ch != '\\' && ch != '\"';
}

static void __dumpref(FILE* f, gc_object_t* o)
{
    fprintf(f, "\"%p\"", (void*)o);
}

static void __dumprefs(FILE* f, tvalue_t* data, size_t size)
{
    bool first = true;

    for (size_t i = 0; i < size; ++i)
    {
        if (iscollectable(&data[i]))
        {
            if (!first)
                fputc(',', f);
            first = false;

            __dumpref(f, gcvalue(&data[i]));
        }
    }
}

static void __dumpstringdata(FILE* f, const char* data, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        fputc(__safejson(data[i]) ? data[i] : '?', f);
}

static void __dumpstring(FILE* f, tstring_t* ts)
{
    fprintf(f, "{\"type\":\"string\",\"cat\":%d,\"size\":%d,\"data\":\"", ts->memcat, (int)(sizestring(ts->len)));
    __dumpstringdata(f, ts->data, ts->len);
    fprintf(f, "\"}");
}

static void __dumptable(FILE* f, lua_table_t* h)
{
    size_t size = sizeof(lua_table_t) + (h->node == &luaH_dummynode ? 0 : sizenode(h) * sizeof(lua_node_t)) + h->sizearray * sizeof(tvalue_t);

    fprintf(f, "{\"type\":\"table\",\"cat\":%d,\"size\":%d", h->memcat, ((int)(size)));

    if (h->node != &luaH_dummynode)
    {
        fprintf(f, ",\"pairs\":[");

        bool first = true;

        for (int i = 0; i < sizenode(h); ++i)
        {
            const lua_node_t* n = &h->node[i];

            if (!ttisnil(&n->val) && (iscollectable(&n->key) || iscollectable(&n->val)))
            {
                if (!first)
                    fputc(',', f);
                first = false;

                if (iscollectable(&n->key))
                    __dumpref(f, gcvalue(&n->key));
                else
                    fprintf(f, "null");

                fputc(',', f);

                if (iscollectable(&n->val))
                    __dumpref(f, gcvalue(&n->val));
                else
                    fprintf(f, "null");
            }
        }

        fprintf(f, "]");
    }
    if (h->sizearray)
    {
        fprintf(f, ",\"array\":[");
        __dumprefs(f, h->array, h->sizearray);
        fprintf(f, "]");
    }
    if (h->metatable)
    {
        fprintf(f, ",\"metatable\":");
        __dumpref(f, obj2gco(h->metatable));
    }
    fprintf(f, "}");
}

static void __dumpclosure(FILE* f, closure_t* cl)
{
    fprintf(
        f, "{\"type\":\"function\",\"cat\":%d,\"size\":%d", cl->memcat, cl->isC ? (int)(sizeCclosure(cl->nupvalues)) : (int)(sizeLclosure(cl->nupvalues))
    );

    fprintf(f, ",\"env\":");
    __dumpref(f, obj2gco(cl->env));

    if (cl->isC)
    {
        if (cl->c.debugname)
            fprintf(f, ",\"name\":\"%s\"", cl->c.debugname + 0);

        if (cl->nupvalues)
        {
            fprintf(f, ",\"upvalues\":[");
            __dumprefs(f, cl->c.upvals, cl->nupvalues);
            fprintf(f, "]");
        }
    }
    else
    {
        if (cl->l.p->debugname)
            fprintf(f, ",\"name\":\"%s\"", getstr(cl->l.p->debugname));

        fprintf(f, ",\"proto\":");
        __dumpref(f, obj2gco(cl->l.p));
        if (cl->nupvalues)
        {
            fprintf(f, ",\"upvalues\":[");
            __dumprefs(f, cl->l.uprefs, cl->nupvalues);
            fprintf(f, "]");
        }
    }
    fprintf(f, "}");
}

static void __dumpudata(FILE* f, udata_t* u)
{
    fprintf(f, "{\"type\":\"userdata\",\"cat\":%d,\"size\":%d,\"tag\":%d", u->memcat, (int)(sizeudata(u->len)), u->tag);

    if (u->metatable)
    {
        fprintf(f, ",\"metatable\":");
        __dumpref(f, obj2gco(u->metatable));
    }
    fprintf(f, "}");
}

static void __dumpthread(FILE* f, lua_State* th)
{
    size_t size = sizeof(lua_State) + sizeof(tvalue_t) * th->stacksize + sizeof(call_info_t) * th->size_ci;

    fprintf(f, "{\"type\":\"thread\",\"cat\":%d,\"size\":%d", th->memcat, ((int)(size)));

    fprintf(f, ",\"env\":");
    __dumpref(f, obj2gco(th->gt));

    closure_t* tcl = 0;
    proto_t* cip = NULL;
    for (call_info_t* ci = th->base_ci; ci <= th->ci; ++ci)
    {
        if (ttisfunction(ci->func))
        {
            tcl = clvalue(ci->func);
            if (FFlag_LuauCIProto)
                cip = ci->p;
            break;
        }
    }

    if (FFlag_LuauCIProto ? (cip != NULL && cip->source) : (tcl && !tcl->isC && tcl->l.p->source))
    {
        proto_t* p = FFlag_LuauCIProto ? cip : tcl->l.p;

        fprintf(f, ",\"source\":\"");
        __dumpstringdata(f, p->source->data, p->source->len);
        fprintf(f, "\",\"line\":%d", p->linedefined);
    }

    if (th->top > th->stack)
    {
        fprintf(f, ",\"stack\":[");
        __dumprefs(f, th->stack, th->top - th->stack);
        fprintf(f, "]");

        call_info_t* ci = th->base_ci;
        bool first = true;

        fprintf(f, ",\"stacknames\":[");
        for (StkId v = th->stack; v < th->top; ++v)
        {
            if (!iscollectable(v))
                continue;

            while (ci < th->ci && v >= (ci + 1)->func)
                ci++;

            if (!first)
                fputc(',', f);
            first = false;

            if (v == ci->func)
            {
                closure_t* cl = ci_func(ci);

                if (cl->isC)
                {
                    fprintf(f, "\"frame:%s\"", cl->c.debugname ? cl->c.debugname : "[C]");
                }
                else
                {
                    proto_t* p = FFlag_LuauCIProto ? ci->p : cl->l.p;
                    fprintf(f, "\"frame:");
                    if (p->source)
                        __dumpstringdata(f, p->source->data, p->source->len);
                    fprintf(f, ":%d:%s\"", p->linedefined, p->debugname ? getstr(p->debugname) : "");
                }
            }
            else if (isLua(ci))
            {
                proto_t* p = FFlag_LuauCIProto ? ci->p : ci_func(ci)->l.p;
                int pc = pcRel(ci->savedpc, p);
                const local_var_t* var = luaF_findlocal(p, ((int)(v - ci->base)), pc);

                if (var && var->varname)
                    fprintf(f, "\"%s\"", getstr(var->varname));
                else
                    fprintf(f, "null");
            }
            else
                fprintf(f, "null");
        }
        fprintf(f, "]");
    }
    fprintf(f, "}");
}

static void __dumpbuffer(FILE* f, luauc_vm_buffer_t* b)
{
    fprintf(f, "{\"type\":\"buffer\",\"cat\":%d,\"size\":%d}", b->memcat, (int)(sizebuffer(b->len)));
}

static void __dumpproto(FILE* f, proto_t* p)
{
    size_t size = sizeof(proto_t) + sizeof(Instruction) * p->sizecode + sizeof(proto_t*) * p->sizep + sizeof(tvalue_t) * p->sizek + p->sizelineinfo +
                  sizeof(local_var_t) * p->sizelocvars + sizeof(tstring_t*) * p->sizeupvalues;

    fprintf(f, "{\"type\":\"proto\",\"cat\":%d,\"size\":%d", p->memcat, ((int)(size)));

    if (p->source)
    {
        fprintf(f, ",\"source\":\"");
        __dumpstringdata(f, p->source->data, p->source->len);
        fprintf(f, "\",\"line\":%d", p->abslineinfo ? p->abslineinfo[0] : 0);
    }

    if (p->sizek)
    {
        fprintf(f, ",\"constants\":[");
        __dumprefs(f, p->k, p->sizek);
        fprintf(f, "]");
    }

    if (p->sizep)
    {
        fprintf(f, ",\"protos\":[");
        for (int i = 0; i < p->sizep; ++i)
        {
            if (i != 0)
                fputc(',', f);
            __dumpref(f, obj2gco(p->p[i]));
        }
        fprintf(f, "]");
    }

    fprintf(f, "}");
}

static void __dumpupval(FILE* f, upvalue_t* uv)
{
    fprintf(f, "{\"type\":\"upvalue\",\"cat\":%d,\"size\":%d,\"open\":%s", uv->memcat, (int)(sizeof(upvalue_t)), upisopen(uv) ? "true" : "false");

    if (iscollectable(uv->v))
    {
        fprintf(f, ",\"object\":");
        __dumpref(f, gcvalue(uv->v));
    }

    fprintf(f, "}");
}

static void __dumpclass(FILE* f, luauc_class_t* lco)
{
    fprintf(f, "{\"type\":\"class\",\"cat\":%d,\"size\":%d", lco->memcat, (int)sizeof(luauc_class_t));
    fprintf(f, ",\"name\":");
    __dumpstringdata(f, lco->name->data, lco->name->len);
    fprintf(f, ",\"membernames\":[");
    for (int i = 0; i < lco->numberofallmembers; i++)
    {
        if (i != 0)
            fputc(',', f);
        __dumpref(f, (gc_object_t*)lco->offsettomember[i]);
    }
    fprintf(f, "],\"staticmembers\":[");
    __dumprefs(f, lco->staticmembers, lco->numberofallmembers - lco->numberofinstancemembers);
    fprintf(f, "],\"metatable\":");
    __dumpref(f, obj2gco(lco->metatable));
    fprintf(f, ",\"instancemetatable\":");
    if (lco->instancemetatable)
        __dumpref(f, obj2gco(lco->instancemetatable));
    else
        fprintf(f, "null");
    fprintf(f, ",\"memberstooffset\":");
    __dumpref(f, obj2gco(lco->memberstooffset));
    fprintf(f, "}");
}

static void __dumpobject(FILE* f, luauc_object_t* inst)
{
    fprintf(f, "{\"type\":\"object\",\"cat\":%d,\"size\":%d", inst->memcat, (int)sizeof(luauc_object_t));
    fprintf(f, ",\"class\":");
    __dumpref(f, obj2gco(inst->lclass));
    fprintf(f, ",\"members\":[");
    __dumprefs(f, inst->members, inst->numberofmembers);
    fprintf(f, "]}");
}

static void __dumpobj(FILE* f, gc_object_t* o)
{
    switch (o->gch.tt)
    {
    case LUA_TSTRING:
        __dumpstring(f, gco2ts(o));
        break;

    case LUA_TTABLE:
        __dumptable(f, gco2h(o));
        break;

    case LUA_TFUNCTION:
        __dumpclosure(f, gco2cl(o));
        break;

    case LUA_TUSERDATA:
        __dumpudata(f, gco2u(o));
        break;

    case LUA_TTHREAD:
        __dumpthread(f, gco2th(o));
        break;

    case LUA_TBUFFER:
        __dumpbuffer(f, gco2buf(o));
        break;

    case LUA_TCLASS:
        __dumpclass(f, gco2class(o));
        break;

    case LUA_TOBJECT:
        __dumpobject(f, gco2object(o));
        break;

    case LUA_TPROTO:
        __dumpproto(f, gco2p(o));
        break;

    case LUA_TUPVAL:
        __dumpupval(f, gco2uv(o));
        break;

    default:
        LUAU_ASSERT(0);
    }
}

static bool __dumpgco(void* context, lua_page_t* page, gc_object_t* gco)
{
    (void)page;
    FILE* f = (FILE*)context;

    __dumpref(f, gco);
    fputc(':', f);
    __dumpobj(f, gco);
    fputc(',', f);
    fputc('\n', f);

    return false;
}

void luaC_dump(lua_State* L, void* file, const char* (*categoryName)(lua_State* L, uint8_t memcat))
{
    global_state_t* g = L->global;
    FILE* f = ((FILE*)(file));

    fprintf(f, "{\"objects\":{\n");

    __dumpgco(f, NULL, obj2gco(g->mainthread));

    luaM_visitgco(L, f, __dumpgco);

    fprintf(f, "\"0\":{\"type\":\"userdata\",\"cat\":0,\"size\":0}\n"); // to avoid issues with trailing ,
    fprintf(f, "},\"roots\":{\n");
    fprintf(f, "\"mainthread\":");
    __dumpref(f, obj2gco(g->mainthread));
    fprintf(f, ",\"registry\":");
    __dumpref(f, gcvalue(&g->registry));

    fprintf(f, "},\"stats\":{\n");

    fprintf(f, "\"size\":%d,\n", ((int)(g->totalbytes)));

    fprintf(f, "\"categories\":{\n");
    for (int i = 0; i < LUA_MEMORY_CATEGORIES; i++)
    {
        size_t bytes = g->memcatbytes[i];
        if (bytes)
        {
            if (categoryName)
                fprintf(f, "\"%d\":{\"name\":\"%s\", \"size\":%d},\n", i, categoryName(L, (uint8_t)i), ((int)(bytes)));
            else
                fprintf(f, "\"%d\":{\"size\":%d},\n", i, ((int)(bytes)));
        }
    }
    fprintf(f, "\"none\":{}\n"); // to avoid issues with trailing ,
    fprintf(f, "}\n");
    fprintf(f, "}}\n");
}

typedef struct enum_context_t
{
    lua_State* L;
    void* context;
    void (*node)(void* context, void* ptr, uint8_t tt, uint8_t memcat, size_t size, const char* name);
    void (*edge)(void* context, void* from, void* to, const char* name);
} enum_context_t;

static void* __enumtopointer(gc_object_t* gco)
{
    // To match lua_topointer, userdata pointer is represented as a pointer to internal data
    return gco->gch.tt == LUA_TUSERDATA ? (void*)gco2u(gco)->data : (void*)gco;
}

static void __enumnode(enum_context_t* ctx, gc_object_t* gco, size_t size, const char* objname)
{
    ctx->node(ctx->context, __enumtopointer(gco), gco->gch.tt, gco->gch.memcat, size, objname);
}

static void __enumedge(enum_context_t* ctx, gc_object_t* from, gc_object_t* to, const char* edgename)
{
    ctx->edge(ctx->context, __enumtopointer(from), __enumtopointer(to), edgename);
}

static void __enumedges(enum_context_t* ctx, gc_object_t* from, tvalue_t* data, size_t size, const char* edgename)
{
    for (size_t i = 0; i < size; ++i)
    {
        if (iscollectable(&data[i]))
            __enumedge(ctx, from, gcvalue(&data[i]), edgename);
    }
}

static void __enumstring(enum_context_t* ctx, tstring_t* ts)
{
    __enumnode(ctx, obj2gco(ts), sizestring(ts->len), NULL);
}

static void __enumtable(enum_context_t* ctx, lua_table_t* h)
{
    size_t size = sizeof(lua_table_t) + (h->node == &luaH_dummynode ? 0 : sizenode(h) * sizeof(lua_node_t)) + h->sizearray * sizeof(tvalue_t);

    // Provide a name for a special registry table
    __enumnode(ctx, obj2gco(h), size, h == hvalue(registry(ctx->L)) ? "registry" : NULL);

    if (h->node != &luaH_dummynode)
    {
        bool weakkey = false;
        bool weakvalue = false;

        const tvalue_t* mode = gfasttm(ctx->L->global, h->metatable, TM_MODE);
        if (mode)
        {
            if (ttisstring(mode))
            {
                weakkey = strchr(svalue(mode), 'k') != NULL;
                weakvalue = strchr(svalue(mode), 'v') != NULL;
            }
        }

        for (int i = 0; i < sizenode(h); ++i)
        {
            const lua_node_t* n = &h->node[i];

            if (!ttisnil(&n->val) && (iscollectable(&n->key) || iscollectable(&n->val)))
            {
                if (!weakkey && iscollectable(&n->key))
                    __enumedge(ctx, obj2gco(h), gcvalue(&n->key), "[key]");

                if (!weakvalue && iscollectable(&n->val))
                {
                    if (ttisstring(&n->key))
                    {
                        __enumedge(ctx, obj2gco(h), gcvalue(&n->val), svalue(&n->key));
                    }
                    else if (ttisnumber(&n->key))
                    {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.14g", nvalue(&n->key));
                        __enumedge(ctx, obj2gco(h), gcvalue(&n->val), buf);
                    }
                    else
                    {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "[%s]", getstr(ctx->L->global->ttname[n->key.tt]));
                        __enumedge(ctx, obj2gco(h), gcvalue(&n->val), buf);
                    }
                }
            }
        }
    }

    if (h->sizearray)
        __enumedges(ctx, obj2gco(h), h->array, h->sizearray, "array");

    if (h->metatable)
        __enumedge(ctx, obj2gco(h), obj2gco(h->metatable), "metatable");
}

static void __enumclosure(enum_context_t* ctx, closure_t* cl)
{
    if (cl->isC)
    {
        __enumnode(ctx, obj2gco(cl), sizeCclosure(cl->nupvalues), cl->c.debugname);
    }
    else
    {
        proto_t* p = cl->l.p;

        char buf[LUA_IDSIZE];

        if (p->source)
            snprintf(buf, sizeof(buf), "%s:%d %s", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined, getstr(p->source));
        else
            snprintf(buf, sizeof(buf), "%s:%d", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined);

        __enumnode(ctx, obj2gco(cl), sizeLclosure(cl->nupvalues), buf);
    }

    __enumedge(ctx, obj2gco(cl), obj2gco(cl->env), "env");

    if (cl->isC)
    {
        if (cl->nupvalues)
            __enumedges(ctx, obj2gco(cl), cl->c.upvals, cl->nupvalues, "upvalue");
    }
    else
    {
        __enumedge(ctx, obj2gco(cl), obj2gco(cl->l.p), "proto");

        if (cl->nupvalues)
            __enumedges(ctx, obj2gco(cl), cl->l.uprefs, cl->nupvalues, "upvalue");
    }
}

static void __enumudata(enum_context_t* ctx, udata_t* u)
{
    const char* name = NULL;

    lua_table_t* h = u->metatable;
    if (h)
    {
        if (h->node != &luaH_dummynode)
        {
            for (int i = 0; i < sizenode(h); ++i)
            {
                const lua_node_t* n = &h->node[i];

                if (ttisstring(&n->key) && ttisstring(&n->val) && strcmp(svalue(&n->key), "__type") == 0)
                {
                    name = svalue(&n->val);
                    break;
                }
            }
        }
    }

    __enumnode(ctx, obj2gco(u), sizeudata(u->len), name);

    if (u->metatable)
        __enumedge(ctx, obj2gco(u), obj2gco(u->metatable), "metatable");
}

static void __enumthread(enum_context_t* ctx, lua_State* th)
{
    size_t size = sizeof(lua_State) + sizeof(tvalue_t) * th->stacksize + sizeof(call_info_t) * th->size_ci;

    closure_t* tcl = NULL;
    proto_t* cip = NULL;
    for (call_info_t* ci = th->base_ci; ci <= th->ci; ++ci)
    {
        if (ttisfunction(ci->func))
        {
            tcl = clvalue(ci->func);
            if (FFlag_LuauCIProto)
                cip = ci->p;
            break;
        }
    }

    if (FFlag_LuauCIProto ? (cip && cip->source) : (tcl && !tcl->isC && tcl->l.p->source))
    {
        proto_t* p = (FFlag_LuauCIProto ? cip : tcl->l.p);

        char buf[LUA_IDSIZE];

        if (p->source)
            snprintf(buf, sizeof(buf), "thread at %s:%d %s", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined, getstr(p->source));
        else
            snprintf(buf, sizeof(buf), "thread at %s:%d", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined);

        __enumnode(ctx, obj2gco(th), size, buf);
    }
    else
    {
        __enumnode(ctx, obj2gco(th), size, NULL);
    }

    __enumedge(ctx, obj2gco(th), obj2gco(th->gt), "globals");

    if (th->top > th->stack)
        __enumedges(ctx, obj2gco(th), th->stack, th->top - th->stack, "stack");
}

static void __enumbuffer(enum_context_t* ctx, luauc_vm_buffer_t* b)
{
    __enumnode(ctx, obj2gco(b), sizebuffer(b->len), NULL);
}

static void __enumproto(enum_context_t* ctx, proto_t* p)
{
    size_t size = sizeof(proto_t) + sizeof(Instruction) * p->sizecode + sizeof(proto_t*) * p->sizep + sizeof(tvalue_t) * p->sizek + p->sizelineinfo +
                  sizeof(local_var_t) * p->sizelocvars + sizeof(tstring_t*) * p->sizeupvalues;

    if (p->execdata && ctx->L->global->ecb.getmemorysize)
    {
        size_t nativesize = ctx->L->global->ecb.getmemorysize(ctx->L, p);

        ctx->node(ctx->context, p->execdata, ((uint8_t)(LUA_TNONE)), p->memcat, nativesize, NULL);
        ctx->edge(ctx->context, __enumtopointer(obj2gco(p)), p->execdata, "[native]");
    }

    char buf[LUA_IDSIZE];

    if (p->source)
        snprintf(buf, sizeof(buf), "proto %s:%d %s", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined, getstr(p->source));
    else
        snprintf(buf, sizeof(buf), "proto %s:%d", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined);

    __enumnode(ctx, obj2gco(p), size, buf);

    if (p->sizek)
        __enumedges(ctx, obj2gco(p), p->k, p->sizek, "constants");

    for (int i = 0; i < p->sizep; ++i)
        __enumedge(ctx, obj2gco(p), obj2gco(p->p[i]), "protos");
}

static void __enumupval(enum_context_t* ctx, upvalue_t* uv)
{
    __enumnode(ctx, obj2gco(uv), sizeof(upvalue_t), NULL);

    if (iscollectable(uv->v))
        __enumedge(ctx, obj2gco(uv), gcvalue(uv->v), "value");
}

static void __enumclass(enum_context_t* ctx, luauc_class_t* lco)
{
    char buf[LUA_IDSIZE];
    gc_object_t* obj = obj2gco(lco);
    snprintf(buf, sizeof(buf), "class object %s", getstr(lco->name));
    __enumnode(ctx, obj, sizeof(luauc_class_t), buf);
    __enumedge(ctx, obj, obj2gco(lco->name), "classname");
    __enumedge(ctx, obj, obj2gco(lco->memberstooffset), "classoffsets");
    int numberofstaticmembers = lco->numberofallmembers - lco->numberofinstancemembers;
    for (int i = 0; i < numberofstaticmembers; i++)
    {
        // It's a bit strange that if we have a non-collectable static member,
        // we'll just not note it as an edge.
        if (!iscollectable(&lco->staticmembers[i]))
            continue;

        char membername[32];
        snprintf(membername, sizeof(membername), "%s", getstr(lco->offsettomember[i + lco->numberofinstancemembers]));
        __enumedge(ctx, obj, gcvalue(&lco->staticmembers[i]), membername);
    }
    for (int i = 0; i < lco->numberofallmembers; i++)
        __enumedge(ctx, obj, obj2gco(lco->offsettomember[i]), "membername");
    __enumedge(ctx, obj, obj2gco(lco->metatable), "metatable");
}

static void __enumobject(enum_context_t* ctx, luauc_object_t* inst)
{
    char buf[LUA_IDSIZE];
    gc_object_t* obj = obj2gco(inst);
    snprintf(buf, sizeof(buf), "object %s", getstr(inst->lclass->name));
    __enumnode(ctx, obj, sizeof(luauc_object_t), buf);
    for (int i = 0; i < inst->lclass->numberofinstancemembers; i++)
    {
        // It's a bit strange that if we have a non-collectable static member,
        // we'll just not note it as an edge.
        if (!iscollectable(&inst->members[i]))
            continue;

        char membername[32];
        snprintf(membername, sizeof(membername), "%s", getstr(inst->lclass->offsettomember[i]));
        __enumedge(ctx, obj, gcvalue(&inst->members[i]), membername);
    }
}

static void __enumobj(enum_context_t* ctx, gc_object_t* o)
{
    switch (o->gch.tt)
    {
    case LUA_TSTRING:
        __enumstring(ctx, gco2ts(o));
        break;

    case LUA_TTABLE:
        __enumtable(ctx, gco2h(o));
        break;

    case LUA_TFUNCTION:
        __enumclosure(ctx, gco2cl(o));
        break;

    case LUA_TUSERDATA:
        __enumudata(ctx, gco2u(o));
        break;

    case LUA_TTHREAD:
        __enumthread(ctx, gco2th(o));
        break;

    case LUA_TBUFFER:
        __enumbuffer(ctx, gco2buf(o));
        break;

    case LUA_TCLASS:
        __enumclass(ctx, gco2class(o));
        break;

    case LUA_TOBJECT:
        __enumobject(ctx, gco2object(o));
        break;

    case LUA_TPROTO:
        __enumproto(ctx, gco2p(o));
        break;

    case LUA_TUPVAL:
        __enumupval(ctx, gco2uv(o));
        break;

    default:
        LUAU_ASSERT(!"Unknown object tag");
    }
}

static bool __enumgco(void* context, lua_page_t* page, gc_object_t* gco)
{
    (void)page;
    __enumobj((enum_context_t*)context, gco);
    return false;
}

void luaC_enumheap(
    lua_State* L,
    void* context,
    void (*node)(void* context, void* ptr, uint8_t tt, uint8_t memcat, size_t size, const char* name),
    void (*edge)(void* context, void* from, void* to, const char* name)
)
{
    global_state_t* g = L->global;

    enum_context_t ctx;
    ctx.L = L;
    ctx.context = context;
    ctx.node = node;
    ctx.edge = edge;

    __enumgco(&ctx, NULL, obj2gco(g->mainthread));

    luaM_visitgco(L, &ctx, __enumgco);
}
