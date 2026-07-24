// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lualib.h"
#include "lstate.h"
#include "lobject.h"
#include "lapi.h"

static const TValue* check_integer(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TINTEGER);
    return luaA_toobject(L, idx);
}

static int integer_fromstring(lua_State* L)
{
    size_t len;
    const char* str = luaL_checklstring(L, 1, &len);
    luaZ_integer_fromstring(L, str, L->top);
    L->top++;
    return 1;
}

static int integer_dynamic(lua_State* L) {
    const TValue* b = check_integer(L, 1);
    if (ttype(b) == LUA_TINTEGER) {
        setintegersmi(L->top, b->value.l, IntegerMode_Dynamic);
    } else {
        setintegerheap(L->top, (HeapInteger*)b->value.gc, IntegerMode_Dynamic);
    }
    L->top++;
    return 1;
}

#define INTEGER_MODE_WRAP(name, mode_enum, c_type, is_unsigned) \
static int integer_##name(lua_State* L) { \
    const TValue* b = check_integer(L, 1); \
    c_type casted = (c_type)luaZ_integer_get_bottom_64(b); \
    int64_t res_smi = is_unsigned ? (int64_t)(uint64_t)casted : (int64_t)casted; \
    setintegersmi(L->top, res_smi, mode_enum); \
    L->top++; \
    return 1; \
}

INTEGER_MODE_WRAP(i8, IntegerMode_I8, int8_t, false)
INTEGER_MODE_WRAP(u8, IntegerMode_U8, uint8_t, true)
INTEGER_MODE_WRAP(i16, IntegerMode_I16, int16_t, false)
INTEGER_MODE_WRAP(u16, IntegerMode_U16, uint16_t, true)
INTEGER_MODE_WRAP(i32, IntegerMode_I32, int32_t, false)
INTEGER_MODE_WRAP(u32, IntegerMode_U32, uint32_t, true)
INTEGER_MODE_WRAP(i64, IntegerMode_I64, int64_t, false)
INTEGER_MODE_WRAP(u64, IntegerMode_U64, uint64_t, true)

static int integer_add(lua_State* L) {
    const TValue* a = check_integer(L, 1);
    const TValue* b = check_integer(L, 2);
    luaZ_integer_add(L, a, b, L->top);
    L->top++;
    return 1;
}

static int integer_sub(lua_State* L) {
    const TValue* a = check_integer(L, 1);
    const TValue* b = check_integer(L, 2);
    luaZ_integer_sub(L, a, b, L->top);
    L->top++;
    return 1;
}

static int integer_mul(lua_State* L) {
    const TValue* a = check_integer(L, 1);
    const TValue* b = check_integer(L, 2);
    luaZ_integer_mul(L, a, b, L->top);
    L->top++;
    return 1;
}

static int integer_div(lua_State* L) {
    const TValue* a = check_integer(L, 1);
    const TValue* b = check_integer(L, 2);
    luaZ_integer_div(L, a, b, L->top);
    L->top++;
    return 1;
}

static const luaL_Reg integerlib[] = {
    {"fromstring", integer_fromstring},
    {"dynamic", integer_dynamic},
    {"i8", integer_i8},
    {"u8", integer_u8},
    {"i16", integer_i16},
    {"u16", integer_u16},
    {"i32", integer_i32},
    {"u32", integer_u32},
    {"i64", integer_i64},
    {"u64", integer_u64},
    {"add", integer_add},
    {"sub", integer_sub},
    {"mul", integer_mul},
    {"div", integer_div},
    {NULL, NULL},
};

LUALIB_API int luaopen_integer(lua_State* L)
{
    luaL_register(L, LUA_INTEGERLIBNAME, integerlib);
    return 1;
}
