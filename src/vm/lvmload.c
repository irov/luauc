// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lclass.h"
#include "lvm.h"

#include "lstate.h"
#include "ltable.h"
#include "lfunc.h"
#include "lobject.h"
#include "lstring.h"

#include "lgc.h"
#include "lmem.h"
#include "lbytecode.h"
#include "lapi.h"

#include <string.h>

LUAU_FASTFLAGVARIABLE(LuauUdataDirectAccess6)
LUAU_FASTFLAG(LuauCallFeedback)
LUAU_FASTFLAGVARIABLE(LuauCostModel)

typedef struct temp_buffer_t
{
    lua_State* L;
    void* data;
    size_t count;
    size_t elementSize;
} temp_buffer_t;

static void __tempBufferInit(temp_buffer_t* buffer, size_t elementSize)
{
    buffer->L = NULL;
    buffer->data = NULL;
    buffer->count = 0;
    buffer->elementSize = elementSize;
}

static void __tempBufferAllocate(temp_buffer_t* buffer, lua_State* L, size_t count)
{
    LUAU_ASSERT(buffer->L == NULL && buffer->data == NULL);
    if (buffer->elementSize != 0 && count > SIZE_MAX / buffer->elementSize)
        luaM_toobig(L);

    buffer->L = L;
    buffer->count = count;
    buffer->data = luaM_new_(L, count * buffer->elementSize, 0);
}

static void __tempBufferDestroy(temp_buffer_t* buffer)
{
    if (buffer->data)
        luaM_free_(buffer->L, buffer->data, buffer->count * buffer->elementSize, 0);

    buffer->L = NULL;
    buffer->data = NULL;
    buffer->count = 0;
}

#define TEMP_BUFFER_AT(type, buffer, index) (((type*)(buffer)->data)[index])

void luaV_getimport(lua_State* L, lua_table_t* env, tvalue_t* k, StkId res, uint32_t id, bool propagatenil)
{
    int count = id >> 30;
    LUAU_ASSERT(count > 0);

    int id0 = ((int)(id >> 20)) & 1023;
    int id1 = ((int)(id >> 10)) & 1023;
    int id2 = ((int)(id)) & 1023;

    // after the first call to luaV_gettable, res may be invalid, and env may (sometimes) be garbage collected
    // we take care to not use env again and to restore res before every consecutive use
    ptrdiff_t resp = savestack(L, res);

    // global lookup for id0
    tvalue_t g;
    sethvalue(L, &g, env);
    luaV_gettable(L, &g, &k[id0], res);

    // table lookup for id1
    if (count < 2)
        return;

    res = restorestack(L, resp);
    if (!propagatenil || !ttisnil(res))
        luaV_gettable(L, res, &k[id1], res);

    // table lookup for id2
    if (count < 3)
        return;

    res = restorestack(L, resp);
    if (!propagatenil || !ttisnil(res))
        luaV_gettable(L, res, &k[id2], res);
}

static void __readBytes(const char* data, size_t size, size_t* offset, void* result, size_t resultSize)
{
    LUAU_ASSERT(*offset <= size && resultSize <= size - *offset);
    memcpy(result, data + *offset, resultSize);
    *offset += resultSize;
}

static uint8_t __readUint8(const char* data, size_t size, size_t* offset)
{
    uint8_t result;
    __readBytes(data, size, offset, &result, sizeof(result));
    return result;
}

static uint32_t __readUint32(const char* data, size_t size, size_t* offset)
{
    uint32_t result;
    __readBytes(data, size, offset, &result, sizeof(result));
    return result;
}

static int32_t __readInt32(const char* data, size_t size, size_t* offset)
{
    int32_t result;
    __readBytes(data, size, offset, &result, sizeof(result));
    return result;
}

static float __readFloat(const char* data, size_t size, size_t* offset)
{
    float result;
    __readBytes(data, size, offset, &result, sizeof(result));
    return result;
}

static double __readDouble(const char* data, size_t size, size_t* offset)
{
    double result;
    __readBytes(data, size, offset, &result, sizeof(result));
    return result;
}

static unsigned int __readVarInt(const char* data, size_t size, size_t* offset)
{
    unsigned int result = 0;
    unsigned int shift = 0;

    uint8_t byte;

    do
    {
        byte = __readUint8(data, size, offset);
        result |= (byte & 127) << shift;
        shift += 7;
    } while (byte & 128);

    return result;
}

static uint64_t __readVarInt64(const char* data, size_t size, size_t* offset)
{
    uint64_t result = 0;
    unsigned int shift = 0;

    uint8_t byte;

    do
    {
        byte = __readUint8(data, size, offset);
        result |= ((uint64_t)(byte & 127)) << shift;
        shift += 7;
    } while (byte & 128);

    return result;
}

static tstring_t* __readString(temp_buffer_t* strings, const char* data, size_t size, size_t* offset)
{
    unsigned int id = __readVarInt(data, size, offset);

    return id == 0 ? NULL : TEMP_BUFFER_AT(tstring_t*, strings, id - 1);
}

typedef struct resolve_import_t
{
    tvalue_t* k;
    uint32_t id;
} resolve_import_t;

static void __resolveImportRun(lua_State* L, void* ud)
{
    resolve_import_t* self = (resolve_import_t*)ud;

    // note: we call getimport with nil propagation which means that accesses to table chains like A.B.C will resolve in nil
    // this is technically not necessary but it reduces the number of exceptions when loading scripts that rely on getfenv/setfenv for global
    // injection
    // allocate a stack slot so that we can do table lookups
    luaD_checkstack(L, 1);
    setnilvalue(L->top);
    L->top++;

    luaV_getimport(L, L->gt, self->k, L->top - 1, self->id, true);
}

static void __resolveImportSafe(lua_State* L, lua_table_t* env, tvalue_t* k, uint32_t id)
{
    (void)env;

    resolve_import_t ri = {k, id};
    if (L->gt->safeenv)
    {
        // luaD_pcall will make sure that if any C/Lua calls during import resolution fail, the thread state is restored back
        int oldTop = lua_gettop(L);
        int status = luaD_pcall(L, __resolveImportRun, &ri, savestack(L, L->top), 0);
        LUAU_ASSERT(oldTop + 1 == lua_gettop(L)); // if an error occurred, luaD_pcall saves it on stack

        if (status != 0)
        {
            // replace error object with nil
            setnilvalue(L->top - 1);
        }
    }
    else
    {
        setnilvalue(L->top);
        L->top++;
    }
}

static void __remapUserdataTypes(char* data, size_t size, uint8_t* userdataRemapping, uint32_t count)
{
    size_t offset = 0;

    uint32_t typeSize = __readVarInt(data, size, &offset);
    uint32_t upvalCount = __readVarInt(data, size, &offset);
    uint32_t localCount = __readVarInt(data, size, &offset);

    if (typeSize != 0)
    {
        uint8_t* types = (uint8_t*)data + offset;

        // Skip two bytes of function type introduction
        for (uint32_t i = 2; i < typeSize; i++)
        {
            uint32_t index = ((uint32_t)(types[i] - LBC_TYPE_TAGGED_USERDATA_BASE));

            if (index < count)
                types[i] = userdataRemapping[index];
        }

        offset += typeSize;
    }

    if (upvalCount != 0)
    {
        uint8_t* types = (uint8_t*)data + offset;

        for (uint32_t i = 0; i < upvalCount; i++)
        {
            uint32_t index = ((uint32_t)(types[i] - LBC_TYPE_TAGGED_USERDATA_BASE));

            if (index < count)
                types[i] = userdataRemapping[index];
        }

        offset += upvalCount;
    }

    if (localCount != 0)
    {
        for (uint32_t i = 0; i < localCount; i++)
        {
            uint32_t index = ((uint32_t)(data[offset] - LBC_TYPE_TAGGED_USERDATA_BASE));

            if (index < count)
                data[offset] = userdataRemapping[index];

            offset += 2;
            __readVarInt(data, size, &offset);
            __readVarInt(data, size, &offset);
        }
    }

    LUAU_ASSERT(offset == size);
}

static int __loadsafe(
    lua_State* L,
    temp_buffer_t* strings,
    temp_buffer_t* protos,
    temp_buffer_t* nilKeys,
    const char* chunkname,
    const char* data,
    size_t size,
    int env
)
{
    size_t offset = 0;

    uint8_t version = __readUint8(data, size, &offset);


    // 0 means the rest of the bytecode is the error message
    if (version == 0)
    {
        char chunkbuf[LUA_IDSIZE];
        const char* chunkid = luaO_chunkid(chunkbuf, sizeof(chunkbuf), chunkname, strlen(chunkname));
        lua_pushfstring(L, "%s%.*s", chunkid, ((int)(size - offset)), data + offset);
        return 1;
    }

    if (version < LBC_VERSION_MIN || version > LBC_VERSION_MAX)
    {
        char chunkbuf[LUA_IDSIZE];
        const char* chunkid = luaO_chunkid(chunkbuf, sizeof(chunkbuf), chunkname, strlen(chunkname));
        lua_pushfstring(L, "%s: bytecode version mismatch (expected [%d..%d], got %d)", chunkid, LBC_VERSION_MIN, LBC_VERSION_MAX, version);
        return 1;
    }

    uint8_t typesversion = 0;

    if (version >= 4)
    {
        typesversion = __readUint8(data, size, &offset);

        if (typesversion < LBC_TYPE_VERSION_MIN || typesversion > LBC_TYPE_VERSION_MAX)
        {
            char chunkbuf[LUA_IDSIZE];
            const char* chunkid = luaO_chunkid(chunkbuf, sizeof(chunkbuf), chunkname, strlen(chunkname));
            lua_pushfstring(
                L, "%s: bytecode type version mismatch (expected [%d..%d], got %d)", chunkid, LBC_TYPE_VERSION_MIN, LBC_TYPE_VERSION_MAX, typesversion
            );
            return 1;
        }
    }

    // env is 0 for current environment and a stack index otherwise
    lua_table_t* envt = (env == 0) ? L->gt : hvalue(luaA_toobject(L, env));

    tstring_t* source = luaS_new(L, chunkname);

    // string table
    unsigned int stringCount = __readVarInt(data, size, &offset);
    __tempBufferAllocate(strings, L, stringCount);

    for (unsigned int i = 0; i < stringCount; ++i)
    {
        unsigned int length = __readVarInt(data, size, &offset);

        TEMP_BUFFER_AT(tstring_t*, strings, i) = luaS_newlstr(L, data + offset, length);
        offset += length;
    }

    // userdata type remapping table
    // for unknown userdata types, the entry will remap to common 'userdata' type
    enum
    {
        UserdataTypeLimit = LBC_TYPE_TAGGED_USERDATA_END - LBC_TYPE_TAGGED_USERDATA_BASE
    };
    uint8_t userdataRemapping[UserdataTypeLimit];

    if (typesversion == 3)
    {
        memset(userdataRemapping, LBC_TYPE_USERDATA, UserdataTypeLimit);

        uint8_t index = __readUint8(data, size, &offset);

        while (index != 0)
        {
            tstring_t* name = __readString(strings, data, size, &offset);

            if (((uint32_t)(index - 1)) < UserdataTypeLimit)
            {
                uint8_t (*cb)(lua_State*, const char*, size_t) = L->global->ecb.gettypemapping;
                if (cb)
                    userdataRemapping[index - 1] = cb(L, getstr(name), name->len);
            }

            index = __readUint8(data, size, &offset);
        }
    }

    // proto table
    unsigned int protoCount = __readVarInt(data, size, &offset);
    __tempBufferAllocate(protos, L, protoCount);

    for (unsigned int i = 0; i < protoCount; ++i)
    {
        uint32_t protoSize = 0;
        if (version >= 12)
            protoSize = __readVarInt(data, size, &offset);
        size_t protoStartOffset = offset;
        proto_t* p = luaF_newproto(L);
        p->source = source;
        p->bytecodeid = ((int)(i));
        p->funid = L->global->lastprotoid == 0 ? 0 : L->global->lastprotoid++;

        p->maxstacksize = __readUint8(data, size, &offset);
        p->numparams = __readUint8(data, size, &offset);
        p->nups = __readUint8(data, size, &offset);
        p->is_vararg = __readUint8(data, size, &offset);

        if (version >= 4)
        {
            p->flags = __readUint8(data, size, &offset);

            if (typesversion == 1)
            {
                uint32_t typesize = __readVarInt(data, size, &offset);

                if (typesize)
                {
                    uint8_t* types = (uint8_t*)data + offset;

                    LUAU_ASSERT(typesize == ((unsigned)(2 + p->numparams)));
                    LUAU_ASSERT(types[0] == LBC_TYPE_FUNCTION);
                    LUAU_ASSERT(types[1] == p->numparams);

                    // transform v1 into v2 format
                    int headersize = typesize > 127 ? 4 : 3;

                    p->typeinfo = luaM_newarray(L, headersize + typesize, uint8_t, p->memcat);
                    p->sizetypeinfo = headersize + typesize;

                    if (headersize == 4)
                    {
                        p->typeinfo[0] = (typesize & 127) | (1 << 7);
                        p->typeinfo[1] = typesize >> 7;
                        p->typeinfo[2] = 0;
                        p->typeinfo[3] = 0;
                    }
                    else
                    {
                        p->typeinfo[0] = ((uint8_t)(typesize));
                        p->typeinfo[1] = 0;
                        p->typeinfo[2] = 0;
                    }

                    memcpy(p->typeinfo + headersize, types, typesize);
                }

                offset += typesize;
            }
            else if (typesversion == 2 || typesversion == 3)
            {
                uint32_t typesize = __readVarInt(data, size, &offset);

                if (typesize)
                {
                    uint8_t* types = (uint8_t*)data + offset;

                    p->typeinfo = luaM_newarray(L, typesize, uint8_t, p->memcat);
                    p->sizetypeinfo = typesize;
                    memcpy(p->typeinfo, types, typesize);
                    offset += typesize;

                    if (typesversion == 3)
                    {
                        __remapUserdataTypes((char*)(uint8_t*)p->typeinfo, p->sizetypeinfo, userdataRemapping, UserdataTypeLimit);
                    }
                }
            }
        }

        const int sizecode = __readVarInt(data, size, &offset);
        p->code = luaM_newarray(L, sizecode, Instruction, p->memcat);
        p->sizecode = sizecode;

        for (int j = 0; j < p->sizecode; ++j)
            p->code[j] = __readUint32(data, size, &offset);

        p->codeentry = p->code;

        const int sizek = __readVarInt(data, size, &offset);
        p->k = luaM_newarray(L, sizek, tvalue_t, p->memcat);
        p->sizek = sizek;

        // Initialize the constants to nil to ensure they have a valid state
        // in the event that some operation in the following loop fails with
        // an exception.
        for (int j = 0; j < p->sizek; ++j)
        {
            setnilvalue(&p->k[j]);
        }

        for (int j = 0; j < p->sizek; ++j)
        {
            switch (__readUint8(data, size, &offset))
            {
            case LBC_CONSTANT_NIL:
                // All constants have already been pre-initialized to nil
                break;

            case LBC_CONSTANT_BOOLEAN:
            {
                uint8_t v = __readUint8(data, size, &offset);
                setbvalue(&p->k[j], v);
                break;
            }

            case LBC_CONSTANT_NUMBER:
            {
                double v = __readDouble(data, size, &offset);
                setnvalue(&p->k[j], v);
                break;
            }

            case LBC_CONSTANT_VECTOR:
            {
                float x = __readFloat(data, size, &offset);
                float y = __readFloat(data, size, &offset);
                float z = __readFloat(data, size, &offset);
                float w = __readFloat(data, size, &offset);
                (void)w;
                setvvalue(&p->k[j], x, y, z, w);
                break;
            }

            case LBC_CONSTANT_STRING:
            {
                tstring_t* v = __readString(strings, data, size, &offset);
                setsvalue(L, &p->k[j], v);
                break;
            }

            case LBC_CONSTANT_IMPORT:
            {
                uint32_t iid = __readUint32(data, size, &offset);
                __resolveImportSafe(L, envt, p->k, iid);
                setobj(L, &p->k[j], L->top - 1);
                L->top--;
                break;
            }

            case LBC_CONSTANT_TABLE:
            {
                int keys = __readVarInt(data, size, &offset);
                lua_table_t* h = luaH_new(L, 0, keys);
                for (int i = 0; i < keys; ++i)
                {
                    int key = __readVarInt(data, size, &offset);
                    tvalue_t* val = luaH_set(L, h, &p->k[key]);
                    setnvalue(val, 0.0);
                }
                sethvalue(L, &p->k[j], h);
                break;
            }

            case LBC_CONSTANT_TABLE_WITH_CONSTANTS:
            {
                uint32_t keys = __readVarInt(data, size, &offset);
                lua_table_t* h = luaH_new(L, 0, keys);

                __tempBufferAllocate(nilKeys, L, keys);
                size_t nilKeysSize = 0;

                for (uint32_t i = 0; i < keys; ++i)
                {
                    int32_t key = __readVarInt(data, size, &offset);
                    tvalue_t* val = luaH_set(L, h, &p->k[key]);
                    int32_t constantIdx = __readInt32(data, size, &offset);
                    if (constantIdx >= 0)
                    {
                        tvalue_t* constant = &p->k[constantIdx];
                        if (ttisnil(constant))
                        {
                            TEMP_BUFFER_AT(int32_t, nilKeys, nilKeysSize++) = key;
                        }
                        else
                        {
                            setobj2t(L, val, constant);
                            luaC_barriert(L, h, constant);
                            continue;
                        }
                    }
                    setnvalue(val, 0.0);
                }

                for (size_t idx = 0; idx < nilKeysSize; idx++)
                {
                    int32_t key = TEMP_BUFFER_AT(int32_t, nilKeys, idx);
                    tvalue_t* val = luaH_set(L, h, &p->k[key]);
                    setnilvalue(val);
                }

                __tempBufferDestroy(nilKeys);
                sethvalue(L, &p->k[j], h);
                break;
            }

            case LBC_CONSTANT_CLOSURE:
            {
                uint32_t fid = __readVarInt(data, size, &offset);
                proto_t* child = TEMP_BUFFER_AT(proto_t*, protos, fid);
                closure_t* cl = luaF_newLclosure(L, child->nups, envt, child);
                cl->preload = (cl->nupvalues > 0);
                setclvalue(L, &p->k[j], cl);
                break;
            }

            case LBC_CONSTANT_CLASS_SHAPE:
            {
                uint32_t cnid = __readVarInt(data, size, &offset);
                tvalue_t* classname = &p->k[cnid];
                LUAU_ASSERT(ttisstring(classname));
                uint32_t numProperties = __readVarInt(data, size, &offset);
                uint32_t numMethods = __readVarInt(data, size, &offset);
                uint32_t numMembers = numMethods + numProperties;
                tstring_t** offsetToMember = luaM_newarray(L, numMembers, tstring_t*, L->activememcat);
                lua_table_t* membersToOffset = luaH_new(L, 0, numMembers);

                for (uint32_t idx = 0; idx < numMembers; idx++)
                {
                    uint32_t mid = __readVarInt(data, size, &offset);
                    tvalue_t* memberName = &p->k[mid];
                    LUAU_ASSERT(ttisstring(memberName));
                    offsetToMember[idx] = tsvalue(memberName);
                    tvalue_t* val = luaH_setstr(L, membersToOffset, tsvalue(memberName));
                    setnvalue(val, idx);
                }

                membersToOffset->readonly = true;

                luauc_class_t* lco = luaR_newclass(L, tsvalue(classname), membersToOffset, offsetToMember, numProperties, numMethods);
                setclassvalue(L, &p->k[j], lco);
                break;
            }

            case LBC_CONSTANT_INTEGER:
            {
                bool isNegative = __readUint8(data, size, &offset);
                uint64_t magnitude = __readVarInt64(data, size, &offset);
                setlvalue(&p->k[j], isNegative ? (int64_t)(~magnitude + 1) : (int64_t)magnitude);
                break;
            }

            default:
                LUAU_ASSERT(!"Unexpected constant kind");
            }
        }

        if (FFlag_LuauUdataDirectAccess6)
        {
            for (Instruction* instruction = p->code; instruction < p->code + p->sizecode;)
            {
                int targetOp = -1;

                switch (LUAU_INSN_OP(*instruction))
                {
                case LOP_GETTABLEKS:
                    targetOp = LOP_GETUDATAKS;
                    break;

                case LOP_SETTABLEKS:
                    targetOp = LOP_SETUDATAKS;
                    break;

                case LOP_NAMECALL:
                    targetOp = LOP_NAMECALLUDATA;
                    break;
                }

                if (targetOp != -1)
                {
                    LUAU_ASSERT(instruction[1] < ((uint32_t)(sizek)));

                    // We take over the upper 16 bits of AUX - so no constants with big indices.
                    if (instruction[1] < 0x10000)
                    {
                        tvalue_t* k = &p->k[instruction[1]];
                        tstring_t* s = tsvalue(k);

                        luaS_updateatom(L, s);

                        if (s->atom >= 0)
                            *instruction = (*instruction & 0xffffff00) | targetOp;
                    }
                }

                instruction += __luauc_get_op_length((luauc_opcode_t)LUAU_INSN_OP(*instruction));
            }
        }

        const int sizep = __readVarInt(data, size, &offset);
        p->p = luaM_newarray(L, sizep, proto_t*, p->memcat);
        p->sizep = sizep;

        for (int j = 0; j < p->sizep; ++j)
        {
            uint32_t fid = __readVarInt(data, size, &offset);
            p->p[j] = TEMP_BUFFER_AT(proto_t*, protos, fid);
        }

        p->linedefined = __readVarInt(data, size, &offset);
        p->debugname = __readString(strings, data, size, &offset);

        uint8_t lineinfo = __readUint8(data, size, &offset);

        if (lineinfo)
        {
            p->linegaplog2 = __readUint8(data, size, &offset);

            int intervals = ((p->sizecode - 1) >> p->linegaplog2) + 1;
            int absoffset = (p->sizecode + 3) & ~3;

            const int sizelineinfo = absoffset + intervals * sizeof(int);
            p->lineinfo = luaM_newarray(L, sizelineinfo, uint8_t, p->memcat);
            p->sizelineinfo = sizelineinfo;

            p->abslineinfo = (int*)(p->lineinfo + absoffset);

            uint8_t lastoffset = 0;
            for (int j = 0; j < p->sizecode; ++j)
            {
                lastoffset += __readUint8(data, size, &offset);
                p->lineinfo[j] = lastoffset;
            }

            int lastline = 0;
            for (int j = 0; j < intervals; ++j)
            {
                lastline += __readInt32(data, size, &offset);
                p->abslineinfo[j] = lastline;
            }
        }

        uint8_t debuginfo = __readUint8(data, size, &offset);

        if (debuginfo)
        {
            const int sizelocvars = __readVarInt(data, size, &offset);
            p->locvars = luaM_newarray(L, sizelocvars, local_var_t, p->memcat);
            p->sizelocvars = sizelocvars;

            for (int j = 0; j < p->sizelocvars; ++j)
            {
                p->locvars[j].varname = __readString(strings, data, size, &offset);
                p->locvars[j].startpc = __readVarInt(data, size, &offset);
                p->locvars[j].endpc = __readVarInt(data, size, &offset);
                p->locvars[j].reg = __readUint8(data, size, &offset);
            }

            const int sizeupvalues = __readVarInt(data, size, &offset);
            LUAU_ASSERT(sizeupvalues == p->nups);

            p->upvalues = luaM_newarray(L, sizeupvalues, tstring_t*, p->memcat);
            p->sizeupvalues = sizeupvalues;

            for (int j = 0; j < p->sizeupvalues; ++j)
            {
                p->upvalues[j] = __readString(strings, data, size, &offset);
            }
        }

        if (version >= 11)
        {
            p->feedbackvecsize = __readVarInt(data, size, &offset);

            if (p->feedbackvecsize > 0)
            {
                p->feedbackvec = luaM_newarray(L, p->feedbackvecsize, feedback_vector_slot_t, p->memcat);
            }
            for (uint32_t j = 0; j < p->feedbackvecsize; j++)
            {
                uint8_t slottype = __readUint8(data, size, &offset);
                LUAU_ASSERT(slottype == LFT_CALLTARGET);
                feedback_vector_slot_t* slot = &p->feedbackvec[j];
                slot->kind = (feedback_vector_slot_kind_t)slottype;
                slot->call_target.pc = __readVarInt(data, size, &offset);
                slot->call_target.proto = 0;
                slot->call_target.hits = 0;
            }
        }

        if (version >= 12)
        {
            if ((p->flags & LPF_INLINABLE) != 0)
                p->cost = __readVarInt64(data, size, &offset);
        }

        if (version >= 12)
        {
            // Potantially skipping unknown data at the end of proto_t.
            offset = protoStartOffset + protoSize;
        }

        TEMP_BUFFER_AT(proto_t*, protos, i) = p;
    }

    // "main" proto is pushed to Lua stack
    uint32_t mainid = __readVarInt(data, size, &offset);
    proto_t* main = TEMP_BUFFER_AT(proto_t*, protos, mainid);

    luaC_threadbarrier(L);

    closure_t* cl = luaF_newLclosure(L, 0, envt, main);
    setclvalue(L, L->top, cl);
    incr_top(L);

    return 0;
}

typedef struct load_context_t
{
    temp_buffer_t strings;
    temp_buffer_t protos;
    temp_buffer_t nilKeys;
    const char* chunkname;
    const char* data;
    size_t size;
    int env;
    int result;
} load_context_t;

static void __loadContextRun(lua_State* L, void* ud)
{
    load_context_t* ctx = (load_context_t*)ud;
    ctx->result = __loadsafe(L, &ctx->strings, &ctx->protos, &ctx->nilKeys, ctx->chunkname, ctx->data, ctx->size, ctx->env);
}

int luau_load(lua_State* L, const char* chunkname, const char* data, size_t size, int env)
{
    // we will allocate a fair amount of memory so check GC before we do
    luaC_checkGC(L);

    load_context_t ctx;
    __tempBufferInit(&ctx.strings, sizeof(tstring_t*));
    __tempBufferInit(&ctx.protos, sizeof(proto_t*));
    __tempBufferInit(&ctx.nilKeys, sizeof(int32_t));
    ctx.chunkname = chunkname;
    ctx.data = data;
    ctx.size = size;
    ctx.env = env;
    ctx.result = 0;

    // pause GC for the duration of deserialization - some objects we're creating aren't rooted
    size_t originalThreshold = L->global->GCthreshold;
    L->global->GCthreshold = SIZE_MAX;

    int status = luaD_rawrunprotected(L, __loadContextRun, &ctx);

    L->global->GCthreshold = originalThreshold;
    __tempBufferDestroy(&ctx.nilKeys);
    __tempBufferDestroy(&ctx.protos);
    __tempBufferDestroy(&ctx.strings);

    // load can either succeed or get an OOM error, any other errors should be handled internally
    LUAU_ASSERT(status == LUA_OK || status == LUA_ERRMEM);

    if (status == LUA_ERRMEM)
    {
        lua_pushstring(L, LUA_MEMERRMSG); // out-of-memory error message doesn't require an allocation
        return 1;
    }

    return ctx.result;
}
