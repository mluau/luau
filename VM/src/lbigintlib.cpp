// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lualib.h"
#include "lstate.h"
#include "lobject.h"
#include "lapi.h"

static BigInt check_bigint(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TBIGINT);
    return bigintvalue(luaA_toobject(L, idx));
}

static int bigint_fromstring(lua_State* L)
{
    size_t len;
    const char* str = luaL_checklstring(L, 1, &len);
    BigInt b = luaZ_bigint_fromstring(L, str);
    setbigintvalue(L->top, b);
    L->top++;
    return 1;
}

static uint64_t get_bottom_64(BigInt b) {
    if (!b.heap) return (uint64_t)b.smi;
    uint64_t val = 0;
    if (b.heap->size > 0) val |= b.heap->digits[0];
    if (b.heap->size > 1) val |= ((uint64_t)b.heap->digits[1] << 32);
    if (b.heap->isNegative) val = ~val + 1;
    return val;
}

static int bigint_dynamic(lua_State* L) {
    BigInt b = check_bigint(L, 1);
    b.mode = BigIntMode_Dynamic;
    setbigintvalue(L->top, b);
    L->top++;
    return 1;
}

#define BIGINT_MODE_WRAP(name, mode_enum, c_type, is_unsigned) \
static int bigint_##name(lua_State* L) { \
    BigInt b = check_bigint(L, 1); \
    c_type casted = (c_type)get_bottom_64(b); \
    int64_t res_smi = is_unsigned ? (int64_t)(uint64_t)casted : (int64_t)casted; \
    BigInt res = luaZ_newbigint(res_smi, mode_enum); \
    setbigintvalue(L->top, res); \
    L->top++; \
    return 1; \
}

BIGINT_MODE_WRAP(i8, BigIntMode_I8, int8_t, false)
BIGINT_MODE_WRAP(u8, BigIntMode_U8, uint8_t, true)
BIGINT_MODE_WRAP(i16, BigIntMode_I16, int16_t, false)
BIGINT_MODE_WRAP(u16, BigIntMode_U16, uint16_t, true)
BIGINT_MODE_WRAP(i32, BigIntMode_I32, int32_t, false)
BIGINT_MODE_WRAP(u32, BigIntMode_U32, uint32_t, true)
BIGINT_MODE_WRAP(i64, BigIntMode_I64, int64_t, false)
BIGINT_MODE_WRAP(u64, BigIntMode_U64, uint64_t, true)

static const luaL_Reg bigintlib[] = {
    {"fromstring", bigint_fromstring},
    {"dynamic", bigint_dynamic},
    {"i8", bigint_i8},
    {"u8", bigint_u8},
    {"i16", bigint_i16},
    {"u16", bigint_u16},
    {"i32", bigint_i32},
    {"u32", bigint_u32},
    {"i64", bigint_i64},
    {"u64", bigint_u64},
    {NULL, NULL},
};

LUALIB_API int luaopen_bigint(lua_State* L)
{
    luaL_register(L, LUA_BIGINTLIBNAME, bigintlib);
    return 1;
}
