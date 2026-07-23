#include "doctest.h"
#include "lua.h"
#include "lualib.h"
#include "lobject.h"
#include <vector>

TEST_SUITE_BEGIN("BigIntTests");

TEST_CASE("BigInt_SMI_Arithmetic") {
    lua_State* L = luaL_newstate();
    
    BigInt a = luaZ_newbigint(5);
    BigInt b = luaZ_newbigint(10);
    
    BigInt sum = luaZ_bigint_add(L, a, b);
    CHECK(sum.heap == nullptr);
    CHECK(sum.smi == 15);
    
    BigInt diff = luaZ_bigint_sub(L, a, b);
    CHECK(diff.heap == nullptr);
    CHECK(diff.smi == -5);
    
    BigInt prod = luaZ_bigint_mul(L, a, b);
    CHECK(prod.heap == nullptr);
    CHECK(prod.smi == 50);
    
    BigInt div = luaZ_bigint_div(L, b, a);
    CHECK(div.heap == nullptr);
    CHECK(div.smi == 2);
    
    BigInt mod = luaZ_bigint_mod(L, luaZ_newbigint(11), luaZ_newbigint(4));
    CHECK(mod.heap == nullptr);
    CHECK(mod.smi == 3);
    
    lua_close(L);
}

TEST_CASE("BigInt_Heap_Multiplication_And_Addition") {
    lua_State* L = luaL_newstate();
    
    // 2^60
    int64_t large_val = 1LL << 60;
    BigInt a = luaZ_newbigint(large_val);
    
    // (2^60) * (2^60) = 2^120
    // This will definitely overflow int64_t and fallback to HeapBigInt
    BigInt prod = luaZ_bigint_mul(L, a, a);
    
    REQUIRE(prod.heap != nullptr);
    CHECK(prod.heap->isNegative == false);
    
    // 2^120 in base 2^32 has digits. 
    // 120 / 32 = 3.75, so 4 digits.
    // 2^120 = (2^24) * (2^32)^3
    // Digits: [0, 0, 0, 2^24]
    REQUIRE(prod.heap->size == 4);
    CHECK(prod.heap->digits[0] == 0);
    CHECK(prod.heap->digits[1] == 0);
    CHECK(prod.heap->digits[2] == 0);
    CHECK(prod.heap->digits[3] == (1U << 24));
    
    // Now test addition on heap bigints
    // (2^120) + (2^120) = 2^121
    BigInt sum = luaZ_bigint_add(L, prod, prod);
    REQUIRE(sum.heap != nullptr);
    REQUIRE(sum.heap->size == 4);
    CHECK(sum.heap->digits[0] == 0);
    CHECK(sum.heap->digits[1] == 0);
    CHECK(sum.heap->digits[2] == 0);
    CHECK(sum.heap->digits[3] == (1U << 25)); // 2^25 * 2^96 = 2^121
    
    // Test subtraction
    // (2^121) - (2^120) = 2^120
    BigInt diff = luaZ_bigint_sub(L, sum, prod);
    REQUIRE(diff.heap != nullptr);
    REQUIRE(diff.heap->size == 4);
    CHECK(diff.heap->digits[3] == (1U << 24));
    
    // Subtraction that yields negative
    // (2^120) - (2^121) = -2^120
    BigInt neg_diff = luaZ_bigint_sub(L, prod, sum);
    REQUIRE(neg_diff.heap != nullptr);
    CHECK(neg_diff.heap->isNegative == true);
    REQUIRE(neg_diff.heap->size == 4);
    CHECK(neg_diff.heap->digits[3] == (1U << 24));

    lua_close(L);
}

TEST_CASE("BigInt_Heap_Division_And_Modulo") {
    lua_State* L = luaL_newstate();
    
    int64_t large_val = 1LL << 60;
    BigInt a = luaZ_newbigint(large_val);
    BigInt num = luaZ_bigint_mul(L, a, a); // 2^120
    
    BigInt div = luaZ_newbigint(3);
    
    // 2^120 / 3
    BigInt q = luaZ_bigint_div(L, num, div);
    REQUIRE(q.heap != nullptr);
    CHECK(q.heap->isNegative == false);
    
    BigInt rem = luaZ_bigint_rem(L, num, div);
    
    // 2^120 mod 3
    // 2 = -1 mod 3 -> 2^120 = (-1)^120 mod 3 = 1 mod 3
    CHECK(rem.heap == nullptr); // Should pack down to SMI!
    CHECK(rem.smi == 1);
    
    lua_close(L);
}

TEST_SUITE_END();
