// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lfunc.h"

#include "lstate.h"
#include "lmem.h"
#include "lgc.h"

LUAU_FASTFLAG(LuauCIProto)
LUAU_FASTINTVARIABLE(LuauInlineHitsThreshold, 32)

proto_t* luaF_newproto(lua_State* L)
{
    proto_t* f = luaM_newgco(L, proto_t, sizeof(proto_t), L->activememcat);

    luaC_init(L, f, LUA_TPROTO);

    f->nups = 0;
    f->numparams = 0;
    f->is_vararg = 0;
    f->maxstacksize = 0;
    f->flags = 0;

    f->k = NULL;
    f->code = NULL;
    f->p = NULL;
    f->codeentry = NULL;

    f->execdata = NULL;
    f->exectarget = 0;

    f->lineinfo = NULL;
    f->abslineinfo = NULL;
    f->locvars = NULL;
    f->upvalues = NULL;
    f->source = NULL;

    f->debugname = NULL;
    f->debuginsn = NULL;

    f->typeinfo = NULL;

    f->userdata = NULL;

    f->gclist = NULL;

    f->sizecode = 0;
    f->sizep = 0;
    f->sizelocvars = 0;
    f->sizeupvalues = 0;
    f->sizek = 0;
    f->sizelineinfo = 0;
    f->linegaplog2 = 0;
    f->linedefined = 0;
    f->bytecodeid = 0;
    f->sizetypeinfo = 0;

    f->feedbackvec = NULL;
    f->feedbackvecsize = 0;
    f->funid = 0;
    f->optimized = NULL;
    f->deoptimized = NULL;
    f->cost = 0;

    return f;
}

closure_t* luaF_newLclosure(lua_State* L, int nelems, lua_table_t* e, proto_t* p)
{
    closure_t* c = luaM_newgco(L, closure_t, sizeLclosure(nelems), L->activememcat);
    luaC_init(L, c, LUA_TFUNCTION);
    c->isC = 0;
    c->env = e;
    c->nupvalues = cast_byte(nelems);
    c->stacksize = p->maxstacksize;
    c->preload = 0;
    c->l.p = p;
    for (int i = 0; i < nelems; ++i)
        setnilvalue(&c->l.uprefs[i]);
    return c;
}

closure_t* luaF_newCclosure(lua_State* L, int nelems, lua_table_t* e)
{
    closure_t* c = luaM_newgco(L, closure_t, sizeCclosure(nelems), L->activememcat);
    luaC_init(L, c, LUA_TFUNCTION);
    c->isC = 1;
    c->env = e;
    c->nupvalues = cast_byte(nelems);
    c->stacksize = LUA_MINSTACK;
    c->preload = 0;
    c->c.f = NULL;
    c->c.cont = NULL;
    c->c.debugname = NULL;
    return c;
}

upvalue_t* luaF_findupval(lua_State* L, StkId level)
{
    global_state_t* g = L->global;
    upvalue_t** pp = &L->openupval;
    upvalue_t* p;
    while (*pp != NULL && (p = *pp)->v >= level)
    {
        LUAU_ASSERT(!isdead(g, obj2gco(p)));
        LUAU_ASSERT(upisopen(p));
        if (p->v == level)
            return p;

        pp = &p->u.open.threadnext;
    }

    LUAU_ASSERT(L->isactive);
    LUAU_ASSERT(!isblack(obj2gco(L))); // we don't use luaC_threadbarrier because active threads never turn black

    upvalue_t* uv = luaM_newgco(L, upvalue_t, sizeof(upvalue_t), L->activememcat); // not found: create a new one
    luaC_init(L, uv, LUA_TUPVAL);
    uv->markedopen = 0;
    uv->v = level; // current value lives in the stack

    // chain the upvalue in the threads open upvalue list at the proper position
    uv->u.open.threadnext = *pp;
    *pp = uv;

    // double link the upvalue in the global open upvalue list
    uv->u.open.prev = &g->uvhead;
    uv->u.open.next = g->uvhead.u.open.next;
    uv->u.open.next->u.open.prev = uv;
    g->uvhead.u.open.next = uv;
    LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);

    return uv;
}

void luaF_freeupval(lua_State* L, upvalue_t* uv, lua_page_t* page)
{
    luaM_freegco(L, uv, sizeof(upvalue_t), uv->memcat, page); // free upvalue
}

void luaF_close(lua_State* L, StkId level)
{
    global_state_t* g = L->global;
    upvalue_t* uv;
    while (L->openupval != NULL && (uv = L->openupval)->v >= level)
    {
        gc_object_t* o = obj2gco(uv);
        LUAU_ASSERT(!isblack(o) && upisopen(uv));
        LUAU_ASSERT(!isdead(g, o));

        // unlink value *before* closing it since value storage overlaps
        L->openupval = uv->u.open.threadnext;

        luaF_closeupval(L, uv, /* dead= */ false);
    }
}

void luaF_closeupval(lua_State* L, upvalue_t* uv, bool dead)
{
    // unlink value from all lists *before* closing it since value storage overlaps
    LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);
    uv->u.open.next->u.open.prev = uv->u.open.prev;
    uv->u.open.prev->u.open.next = uv->u.open.next;

    if (dead)
        return;

    setobj(L, &uv->u.value, uv->v);
    uv->v = &uv->u.value;
    luaC_upvalclosed(L, uv);
}

void luaF_freeproto(lua_State* L, proto_t* f, lua_page_t* page)
{
    luaM_freearray(L, f->code, f->sizecode, Instruction, f->memcat);
    luaM_freearray(L, f->p, f->sizep, proto_t*, f->memcat);
    luaM_freearray(L, f->k, f->sizek, tvalue_t, f->memcat);
    if (f->lineinfo)
        luaM_freearray(L, f->lineinfo, f->sizelineinfo, uint8_t, f->memcat);
    luaM_freearray(L, f->locvars, f->sizelocvars, struct local_var_t, f->memcat);
    luaM_freearray(L, f->upvalues, f->sizeupvalues, tstring_t*, f->memcat);
    if (f->debuginsn)
        luaM_freearray(L, f->debuginsn, f->sizecode, uint8_t, f->memcat);

    if (f->execdata)
        L->global->ecb.destroy(L, f);

    if (f->typeinfo)
        luaM_freearray(L, f->typeinfo, f->sizetypeinfo, uint8_t, f->memcat);

    if (f->feedbackvec)
        luaM_freearray(L, f->feedbackvec, f->feedbackvecsize, feedback_vector_slot_t, f->memcat);

    luaM_freegco(L, f, sizeof(proto_t), f->memcat, page);
}

void luaF_freeclosure(lua_State* L, closure_t* c, lua_page_t* page)
{
    int size = c->isC ? sizeCclosure(c->nupvalues) : sizeLclosure(c->nupvalues);
    luaM_freegco(L, c, size, c->memcat, page);
}

const local_var_t* luaF_getlocal(const proto_t* f, int local_number, int pc)
{
    for (int i = 0; i < f->sizelocvars; i++)
    {
        if (pc >= f->locvars[i].startpc && pc < f->locvars[i].endpc)
        { // is variable active?
            local_number--;
            if (local_number == 0)
                return &f->locvars[i];
        }
    }

    return NULL; // not found
}

const local_var_t* luaF_findlocal(const proto_t* f, int local_reg, int pc)
{
    for (int i = 0; i < f->sizelocvars; i++)
        if (local_reg == f->locvars[i].reg && pc >= f->locvars[i].startpc && pc < f->locvars[i].endpc)
            return &f->locvars[i];

    return NULL; // not found
}

bool luaF_recordhit(lua_State* L, closure_t* caller, closure_t* target, uint32_t slotid)
{
    if (L->global->ecb.inlinefunction == NULL)
        return false;

    LUAU_ASSERT(!caller->isC);
    proto_t* callerp = caller->l.p;
    if (target->isC)
        return false;
    proto_t* targetp = target->l.p;
    LUAU_ASSERT(slotid < callerp->feedbackvecsize);
    feedback_vector_slot_t* slot = &callerp->feedbackvec[slotid];
    LUAU_ASSERT(slot->kind == CALL_TARGET);

    if (slot->call_target.proto == 0)
        slot->call_target.proto = targetp->funid;

    if (slot->call_target.proto != targetp->funid)
        return false;

    slot->call_target.hits++;

    if ((int)slot->call_target.hits >= FInt_LuauInlineHitsThreshold)
    {
        L->global->ecb.inlinefunction(L, caller, target, slot->call_target.pc);
        return false;
    }

    return true;
}
