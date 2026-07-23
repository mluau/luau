// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lualib.h"
#include "lstate.h"
#include "lobject.h"

static int bigint_fromstring(lua_State* L)
{
    size_t len;
    const char* str = luaL_checklstring(L, 1, &len);
    BigInt b = luaZ_bigint_fromstring(L, str);
    setbigintvalue(L->top, b);
    L->top++;
    return 1;
}

static const luaL_Reg bigintlib[] = {
    {"fromstring", bigint_fromstring},
    {NULL, NULL},
};

LUALIB_API int luaopen_bigint(lua_State* L)
{
    luaL_register(L, LUA_BIGINTLIBNAME, bigintlib);
    return 1;
}
