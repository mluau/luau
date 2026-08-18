// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details

#include "lclass.h"

#include "ldebug.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lualib.h"
#include "lvm.h"

LuauClass* luaR_newclass(
    lua_State* L,
    TString* name,
    LuaTable* memberstooffset,
    TString** offsettomember,
    uint8_t* memberflags,
    uint32_t numberofinstancemembers,
    uint32_t numberofstaticmembers
)
{
    LUAU_ASSERT(L->global->GCthreshold == SIZE_MAX && "GC must be paused");
    LuauClass* classdef = luaM_newgco(L, LuauClass, sizeof(LuauClass), L->activememcat);
    luaC_init(L, classdef, LUA_TCLASS);
    classdef->name = name;

    classdef->staticmembers = luaM_newarray(L, numberofstaticmembers, TValue, classdef->memcat);
    // Initialize static members to nil, otherwise we may read uninitialized memory.
    for (uint32_t i = 0; i < numberofstaticmembers; i++)
        setnilvalue(&classdef->staticmembers[i]);

    classdef->memberstooffset = memberstooffset;
    classdef->offsettomember = offsettomember;

    // Initialize the metatable of the _class value_, which for now only
    // contains an __call entry for the class constructor.
    classdef->metatable = luaH_new(L, 0, 1);
    // We should probably pass an empty table here rather than the global
    // environment.
    static const char kCtorSuffix[] = "() constructor";
    size_t namelen = strlen(getstr(name));
    size_t ctordebugnamelen = namelen + sizeof(kCtorSuffix); // includes the null terminator
    classdef->ctordebugname = luaM_newarray(L, ctordebugnamelen, char, classdef->memcat);
    memcpy(classdef->ctordebugname, getstr(name), namelen);
    memcpy(classdef->ctordebugname + namelen, kCtorSuffix, sizeof(kCtorSuffix));

    Closure* constructor = luaF_newCclosure(L, 0, L->gt);
    constructor->c.f = luaR_createobject;
    constructor->c.debugname = classdef->ctordebugname;
    constructor->c.cont = NULL;
    TValue* dest = luaH_setstr(L, classdef->metatable, L->global->tmname[TM_CALL]);
    LUAU_ASSERT(ttisnil(dest));
    setclvalue(L, dest, constructor);
    classdef->metatable->readonly = true;
    classdef->instancemetatable = NULL;

    classdef->numberofinstancemembers = numberofinstancemembers;
    classdef->numberofallmembers = numberofinstancemembers + numberofstaticmembers;
    classdef->hascustominit = false;
    classdef->initoffset = 0;

    classdef->memberflags = memberflags;
    classdef->hasprivatemembers = false;
    classdef->hasconstmembers = false;
    for (uint32_t i = 0; i < classdef->numberofallmembers; i++)
    {
        classdef->hasprivatemembers |= (memberflags[i] & LBC_CLASSMEMBER_PRIVATE) != 0;
        classdef->hasconstmembers |= (memberflags[i] & LBC_CLASSMEMBER_CONST) != 0;
    }

    return classdef;
}

bool luaR_closureownsprivateaccess(const LuauClass* classdef, const Closure* cl)
{
    if (cl->isC)
        return false;

    uint32_t numstaticmembers = classdef->numberofallmembers - classdef->numberofinstancemembers;
    for (uint32_t i = 0; i < numstaticmembers; i++)
    {
        const TValue* v = &classdef->staticmembers[i];
        if (ttisfunction(v) && clvalue(v) == cl)
            return true;
    }

    return false;
}

bool luaR_closureisinit(const LuauClass* classdef, const Closure* cl)
{
    if (cl->isC || !classdef->hascustominit)
        return false;

    const TValue* v = &classdef->staticmembers[classdef->initoffset - classdef->numberofinstancemembers];
    return ttisfunction(v) && clvalue(v) == cl;
}

void luaR_checkprivateaccess(lua_State* L, const TValue* key, const LuauClass* classdef, const Closure* cl, uint32_t offset)
{
    // C API / native callers (cl == NULL, ie no Lua closure is currently executing at all, or
    // cl->isC) are trusted and bypass private/const enforcement entirely -- it's a Luau-script-
    // level access control, not something that should get in the way of embedder code.
    if (!cl || cl->isC)
        return;

    if ((classdef->memberflags[offset] & LBC_CLASSMEMBER_PRIVATE) == 0)
        return;
    if (luaR_closureownsprivateaccess(classdef, cl))
        return;

    luaG_privateaccesserror(L, key, classdef->name);
}

void luaR_checkconstassign(lua_State* L, const TValue* key, const LuauClass* classdef, const Closure* cl, uint32_t offset)
{
    // See luaR_checkprivateaccess: C API / native callers are trusted and bypass this entirely.
    if (!cl || cl->isC)
        return;

    if ((classdef->memberflags[offset] & LBC_CLASSMEMBER_CONST) == 0)
        return;
    if (luaR_closureisinit(classdef, cl))
        return;

    luaG_constassignerror(L, key, classdef->name);
}

void luaR_addclassmember(lua_State* L, LuauClass* classdef, TString* name, TValue* value)
{
    LUAU_ASSERT(classdef->staticmembers != nullptr);
    const TValue* offset = luaH_getstr(classdef->memberstooffset, name);
    const uint32_t offsetint = uint32_t(nvalue(offset));
    LUAU_ASSERT(offsetint >= classdef->numberofinstancemembers && offsetint < classdef->numberofallmembers);
    LUAU_ASSERT(ttisfunction(value) && value->value.gc->gch.tt == LUA_TFUNCTION);
    setobj2class(L, &classdef->staticmembers[offsetint - classdef->numberofinstancemembers], value);
    luaC_barrier(L, classdef, value);

    if (name == luaS_newlstr(L, "__init", 6))
    {
        classdef->hascustominit = true;
        classdef->initoffset = offsetint;
    }

    // Only metamethods in the parser's allowlist are supported (see ALLOWED_METAMETHODS in Parser.cpp)
    bool isMetamethod = (name == luaS_newlstr(L, "__tostring", 10));
    for (int i = 0; i < TM_N && !isMetamethod; i++)
        isMetamethod = (name == L->global->tmname[i]);

    if (isMetamethod)
    {
        if (!classdef->instancemetatable)
        {
            classdef->instancemetatable = luaH_new(L, 0, 1);
            luaC_objbarrier(L, classdef, classdef->instancemetatable);
        }
        TValue* dest = luaH_setstr(L, classdef->instancemetatable, name);
        setobj2t(L, dest, value);
        luaC_barrier(L, classdef->instancemetatable, value);
    }
}

// Initializes the class instance (object) with POD constructor, with L->base + 1 being the stack location we expect
// the user-provided table matching expected fields to values to be. Since classes can have 0 fields that need to be
// initialized we also allow Class() here as well (if class actually had fields they will be nill)
static void luaR_defaultinitinstancefields(lua_State* L, LuauClass* classdef, LuauObject* object, int numargs)
{
    // Stack location to hold the table lookup result
    setnilvalue(L->top);
    L->top++;

    switch (numargs)
    {
    case 1:
        // assume class has 0 fields to initialize or user wants all fields to be nil
        break;
    case 2:
        // by going over the expected instance members instead of the passed table we ensure
        // that users can't add arbitrary properties to the object within the default constructor
        for (uint32_t idx = 0; idx < classdef->numberofinstancemembers; idx++)
        {
            TValue key;
            setsvalue(L, &key, classdef->offsettomember[idx]);
            luaV_gettable(L, L->base + 1, &key, L->top - 1);
            setobj(L, &object->members[idx], L->top - 1);
        }
        break;
    default:
        luaL_error(
            L,
            "the default constructor for constructing a '%s' expected zero or one arguments "
            "(table mapping field names to values or nothing if class has 0 fields), got an incorrect number of arguments",
            getstr(classdef->name)
        );
    }

    L->top--;
}

int luaR_createobject(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TCLASS);
    LuauClass* classdef = classvalue(L->base);

    // Ensure a private constructor is only callable from within its own class.
    if (classdef->hascustominit && classdef->hasprivatemembers &&
        (classdef->memberflags[classdef->initoffset] & LBC_CLASSMEMBER_PRIVATE))
    {
        CallInfo* callerci = L->ci - 1;
        Closure* callercl = nullptr;
        if (isLua(callerci))
            callercl = clvalue(callerci->func);

        TValue initname;
        setsvalue(L, &initname, classdef->offsettomember[classdef->initoffset]);
        luaR_checkprivateaccess(L, &initname, classdef, callercl, classdef->initoffset);
    }

    LuauObject* object = luaM_newgco(L, LuauObject, sizeof(LuauObject), L->activememcat);
    luaC_init(L, object, LUA_TOBJECT);
    object->lclass = classdef;
    object->numberofmembers = classdef->numberofinstancemembers;
    object->members = luaM_newarray(L, object->numberofmembers, TValue, L->activememcat);
    int numargs = lua_gettop(L);

    // We need to initialize all of the instance members to `nil` to start.
    for (uint32_t idx = 0; idx < classdef->numberofinstancemembers; idx++)
        setnilvalue(&object->members[idx]);

    // Push the new object onto the stack. We do this prior to setting the
    // fields as we may reallocate the stack as part of indexing into the
    // second argument (if present).
    setobjectvalue(L, L->top, object);
    L->top++;
    int selfidx = lua_gettop(L);

    if (classdef->hascustominit)
    {
        lua_getfield(L, 1, "__init");
        lua_pushvalue(L, selfidx);
        for (int i = 2; i <= numargs; i++)
            lua_pushvalue(L, i);

        lua_call(L, 1 + (numargs - 1), 0);

        lua_pushvalue(L, selfidx);
        return 1;
    }

    luaR_defaultinitinstancefields(L, classdef, object, numargs);

    // Preserve the GC invariant, moving barrier back once after writing multiple objects (similar to SETLIST)
    luaC_barrierfast(L, object);

    return 1;
}

int luaR_defaultinit(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TOBJECT);
    LuauObject* object = objectvalue(L->base);
    LuauClass* classdef = object->lclass;
    int numargs = lua_gettop(L);

    for (uint32_t idx = 0; idx < classdef->numberofinstancemembers; idx++)
        setnilvalue(&object->members[idx]);

    luaR_defaultinitinstancefields(L, classdef, object, numargs);

    luaC_barrierfast(L, object);

    return 0;
}

void luaR_adddefaultinit(lua_State* L, LuauClass* classdef)
{
    const TValue* offset = luaH_getstr(classdef->memberstooffset, luaS_newlstr(L, "__init", 6));
    LUAU_ASSERT(!ttisnil(offset));
    const uint32_t offsetint = uint32_t(nvalue(offset));
    LUAU_ASSERT(offsetint >= classdef->numberofinstancemembers && offsetint < classdef->numberofallmembers);

    Closure* init = luaF_newCclosure(L, 0, L->gt);
    init->c.f = luaR_defaultinit;
    init->c.debugname = "__init";
    init->c.cont = NULL;

    TValue v;
    setclvalue(L, &v, init);
    setobj2class(L, &classdef->staticmembers[offsetint - classdef->numberofinstancemembers], &v);
    luaC_barrier(L, classdef, &v);
}

void luaR_freeclass(lua_State* L, LuauClass* classdef, lua_Page* page)
{
    luaM_freearray(
        L, classdef->staticmembers, classdef->numberofallmembers - classdef->numberofinstancemembers, TValue, classdef->memcat
    );
    luaM_freearray(L, classdef->offsettomember, classdef->numberofallmembers, TString*, classdef->memcat);
    luaM_freearray(L, classdef->memberflags, classdef->numberofallmembers, uint8_t, classdef->memcat);
    luaM_freearray(L, classdef->ctordebugname, strlen(classdef->ctordebugname) + 1, char, classdef->memcat);
    luaM_freegco(L, classdef, sizeof(LuauClass), classdef->memcat, page);
}

void luaR_freeobject(lua_State* L, LuauObject* object, lua_Page* page)
{
    luaM_freearray(L, object->members, object->numberofmembers, TValue, object->memcat);
    luaM_freegco(L, object, sizeof(LuauObject), object->memcat, page);
}
