// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lobject.h"
#include "lstate.h"
#include "lgc.h"
#include "lmem.h"
#include "luaconf.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>


BigInt luaZ_newbigint(int64_t v)
{
    BigInt b;
    b.smi = v;
    b.heap = nullptr;
    return b;
}

static HeapBigInt* lua_newheapbigint(lua_State* L, uint32_t capacity)
{
    HeapBigInt* h = luaM_newgco(L, HeapBigInt, sizeof(HeapBigInt), L->activememcat);
    luaC_init(L, h, LUA_THEAPBIGINT);
    h->capacity = capacity;
    h->size = 0;
    h->isNegative = false;
    if (capacity > 0)
        h->digits = luaM_newarray(L, capacity, uint32_t, L->activememcat);
    else
        h->digits = nullptr;
    return h;
}

void lua_freebigint(lua_State* L, HeapBigInt* h, struct lua_Page* page)
{
    if (h->capacity > 0)
        luaM_freearray(L, h->digits, h->capacity, uint32_t, h->memcat);
    luaM_freegco(L, h, sizeof(HeapBigInt), h->memcat, page);
}

struct BigIntView {
    const uint32_t* digits;
    uint32_t size;
    bool isNegative;
};

static BigIntView get_view(const BigInt& b, uint32_t temp[2]) {
    if (b.heap) {
        return {b.heap->digits, b.heap->size, b.heap->isNegative};
    } else {
        if (b.smi == 0) {
            return {nullptr, 0, false};
        }
        bool isNeg = b.smi < 0;
        uint64_t abs_val = isNeg ? (uint64_t)(-(b.smi + 1)) + 1 : (uint64_t)b.smi;
        temp[0] = (uint32_t)(abs_val & 0xFFFFFFFF);
        temp[1] = (uint32_t)(abs_val >> 32);
        uint32_t sz = temp[1] != 0 ? 2 : 1;
        return {temp, sz, isNeg};
    }
}

bool luaZ_bigint_eq(BigInt a, BigInt b)
{
    uint32_t ta[2], tb[2];
    BigIntView va = get_view(a, ta);
    BigIntView vb = get_view(b, tb);

    if (va.size != vb.size) return false;
    if (va.size == 0) return true;
    if (va.isNegative != vb.isNegative) return false;
    return memcmp(va.digits, vb.digits, va.size * sizeof(uint32_t)) == 0;
}

uint32_t luaZ_bigint_hash(BigInt b)
{
    uint32_t t[2];
    BigIntView v = get_view(b, t);
    
    uint32_t h = v.size ^ (v.isNegative ? 0x80000000 : 0);
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    
    for (uint32_t i = 0; i < v.size; i++) {
        uint32_t k = v.digits[i];
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
    }
    
    return h;
}


static void normalize(HeapBigInt* h) {
    while (h->size > 0 && h->digits[h->size - 1] == 0) {
        h->size--;
    }
    if (h->size == 0) {
        h->isNegative = false;
    }
}

static BigInt pack_bigint(lua_State* L, HeapBigInt* h) {
    normalize(h);
    if (h->size == 0) {
        return luaZ_newbigint(0);
    }
    if (h->size <= 2) {
        uint64_t val = h->digits[0];
        if (h->size == 2) {
            val |= ((uint64_t)h->digits[1] << 32);
        }
        if (!h->isNegative && val <= (uint64_t)INT64_MAX) {
            return luaZ_newbigint((int64_t)val);
        } else if (h->isNegative && val <= (uint64_t)INT64_MAX + 1) {
            return luaZ_newbigint(val == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)val);
        }
    }
    BigInt b;
    b.smi = 0;
    b.heap = h;
    return b;
}

static int cmp_abs(const BigIntView& a, const BigIntView& b) {
    if (a.size != b.size) {
        return a.size < b.size ? -1 : 1;
    }
    for (int i = (int)a.size - 1; i >= 0; --i) {
        if (a.digits[i] != b.digits[i]) {
            return a.digits[i] < b.digits[i] ? -1 : 1;
        }
    }
    return 0;
}

static void add_abs(HeapBigInt* res, const BigIntView& a, const BigIntView& b) {
    uint32_t max_size = a.size > b.size ? a.size : b.size;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < max_size || carry; ++i) {
        uint64_t sum = carry;
        if (i < a.size) sum += a.digits[i];
        if (i < b.size) sum += b.digits[i];
        res->digits[res->size++] = (uint32_t)(sum & 0xFFFFFFFF);
        carry = sum >> 32;
    }
}

static void sub_abs(HeapBigInt* res, const BigIntView& a, const BigIntView& b) {
    // Requires a >= b
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < a.size; ++i) {
        uint64_t diff = a.digits[i] - borrow;
        if (i < b.size) diff -= b.digits[i];
        res->digits[res->size++] = (uint32_t)(diff & 0xFFFFFFFF);
        borrow = (diff >> 32) ? 1 : 0;
    }
}

static void mul_abs(HeapBigInt* res, const BigIntView& a, const BigIntView& b) {
    if (a.size == 0 || b.size == 0) return;
    for (uint32_t i = 0; i < a.size; ++i) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < b.size || carry; ++j) {
            uint64_t prod = res->digits[i + j] + carry;
            if (j < b.size) prod += (uint64_t)a.digits[i] * b.digits[j];
            res->digits[i + j] = (uint32_t)(prod & 0xFFFFFFFF);
            carry = prod >> 32;
        }
    }
    res->size = a.size + b.size;
}

static void div_mod_abs(lua_State* L, const BigIntView& n, const BigIntView& d, HeapBigInt* q, HeapBigInt* r) {
    if (d.size == 0) {
        luaG_runerror(L, "attempt to divide by zero");
    }
    int cmp = cmp_abs(n, d);
    if (cmp < 0) {
        if (q) q->size = 0;
        if (r) {
            for (uint32_t i = 0; i < n.size; ++i) r->digits[r->size++] = n.digits[i];
        }
        return;
    }
    if (cmp == 0) {
        if (q) q->digits[q->size++] = 1;
        if (r) r->size = 0;
        return;
    }
    HeapBigInt* rem = lua_newheapbigint(L, n.size);
    rem->size = n.size;
    for(uint32_t i = 0; i < n.size; i++) rem->digits[i] = n.digits[i];

    HeapBigInt* shift_d = lua_newheapbigint(L, n.size + 1);
    
    if (q) q->size = n.size;

    int bit_diff = (n.size - d.size) * 32;
    for (int i = bit_diff + 32; i >= 0; --i) {
        shift_d->size = 0;
        uint32_t word_shift = i / 32;
        uint32_t bit_shift = i % 32;
        uint64_t carry = 0;
        for(uint32_t j = 0; j < word_shift; j++) shift_d->digits[shift_d->size++] = 0;
        for(uint32_t j = 0; j < d.size || carry; j++) {
            uint64_t val = carry;
            if (j < d.size) {
                val += ((uint64_t)d.digits[j] << bit_shift);
            }
            shift_d->digits[shift_d->size++] = (uint32_t)(val & 0xFFFFFFFF);
            carry = val >> 32;
        }
        normalize(shift_d);

        BigIntView r_view = {rem->digits, rem->size, false};
        BigIntView sd_view = {shift_d->digits, shift_d->size, false};
        
        if (cmp_abs(r_view, sd_view) >= 0) {
            rem->size = 0;
            sub_abs(rem, r_view, sd_view);
            normalize(rem);
            if (q) {
                q->digits[word_shift] |= (1u << bit_shift);
            }
        }
    }
    if (r) {
        for(uint32_t i = 0; i < rem->size; i++) r->digits[r->size++] = rem->digits[i];
    }
}

BigInt luaZ_bigint_add(lua_State* L, BigInt a, BigInt b)
{
    if (!a.heap && !b.heap) {
        int64_t sum;
        if (!__builtin_add_overflow(a.smi, b.smi, &sum))
            return luaZ_newbigint(sum);
    }
    
    uint32_t ta[2], tb[2];
    BigIntView va = get_view(a, ta);
    BigIntView vb = get_view(b, tb);
    
    HeapBigInt* res = lua_newheapbigint(L, (va.size > vb.size ? va.size : vb.size) + 1);
    
    if (va.isNegative == vb.isNegative) {
        res->isNegative = va.isNegative;
        add_abs(res, va, vb);
    } else {
        int cmp = cmp_abs(va, vb);
        if (cmp >= 0) {
            res->isNegative = va.isNegative;
            sub_abs(res, va, vb);
        } else {
            res->isNegative = vb.isNegative;
            sub_abs(res, vb, va);
        }
    }
    return pack_bigint(L, res);
}

BigInt luaZ_bigint_sub(lua_State* L, BigInt a, BigInt b)
{
    if (!a.heap && !b.heap) {
        int64_t diff;
        if (!__builtin_sub_overflow(a.smi, b.smi, &diff))
            return luaZ_newbigint(diff);
    }
    
    uint32_t ta[2], tb[2];
    BigIntView va = get_view(a, ta);
    BigIntView vb = get_view(b, tb);
    
    HeapBigInt* res = lua_newheapbigint(L, (va.size > vb.size ? va.size : vb.size) + 1);
    
    if (va.isNegative != vb.isNegative) {
        res->isNegative = va.isNegative;
        add_abs(res, va, vb);
    } else {
        int cmp = cmp_abs(va, vb);
        if (cmp >= 0) {
            res->isNegative = va.isNegative;
            sub_abs(res, va, vb);
        } else {
            res->isNegative = !va.isNegative;
            sub_abs(res, vb, va);
        }
    }
    return pack_bigint(L, res);
}

BigInt luaZ_bigint_mul(lua_State* L, BigInt a, BigInt b)
{
    if (!a.heap && !b.heap) {
        int64_t prod;
        if (!__builtin_mul_overflow(a.smi, b.smi, &prod))
            return luaZ_newbigint(prod);
    }
    
    uint32_t ta[2], tb[2];
    BigIntView va = get_view(a, ta);
    BigIntView vb = get_view(b, tb);
    
    HeapBigInt* res = lua_newheapbigint(L, va.size + vb.size);
    for(uint32_t i=0; i < res->capacity; i++) res->digits[i] = 0; // initialize zero
    
    res->isNegative = va.isNegative != vb.isNegative;
    mul_abs(res, va, vb);
    
    return pack_bigint(L, res);
}

BigInt luaZ_bigint_div(lua_State* L, BigInt a, BigInt b)
{
    if (!a.heap && !b.heap) {
        if (b.smi != 0) {
            // Check for INT64_MIN / -1
            if (a.smi == INT64_MIN && b.smi == -1) {
                // Will overflow int64, fallback to heap
            } else {
                return luaZ_newbigint(a.smi / b.smi);
            }
        } else {
            luaG_runerror(L, "attempt to divide by zero");
        }
    }
    
    uint32_t ta[2], tb[2];
    BigIntView va = get_view(a, ta);
    BigIntView vb = get_view(b, tb);
    
    HeapBigInt* q = lua_newheapbigint(L, va.size);
    for(uint32_t i=0; i < q->capacity; i++) q->digits[i] = 0; // initialize zero
    
    q->isNegative = va.isNegative != vb.isNegative;
    
    div_mod_abs(L, va, vb, q, nullptr);
    
    return pack_bigint(L, q);
}

BigInt luaZ_bigint_mod(lua_State* L, BigInt a, BigInt b)
{
    if (!a.heap && !b.heap) {
        if (b.smi != 0) {
            if (a.smi == INT64_MIN && b.smi == -1) {
                return luaZ_newbigint(0);
            }
            int64_t r = a.smi % b.smi;
            if (r != 0 && (a.smi ^ b.smi) < 0) {
                r += b.smi;
            }
            return luaZ_newbigint(r);
        } else {
            luaG_runerror(L, "attempt to perform 'n%%0'");
        }
    }
    
    uint32_t ta[2], tb[2];
    BigIntView va = get_view(a, ta);
    BigIntView vb = get_view(b, tb);
    
    HeapBigInt* r = lua_newheapbigint(L, vb.size);
    for(uint32_t i=0; i < r->capacity; i++) r->digits[i] = 0; // initialize zero
    
    r->isNegative = va.isNegative;
    div_mod_abs(L, va, vb, nullptr, r);
    normalize(r);

    if (r->size > 0 && va.isNegative != vb.isNegative) {
        // Lua modulo: r = r + b when signs differ and r != 0
        HeapBigInt* res = lua_newheapbigint(L, (r->size > vb.size ? r->size : vb.size) + 1);
        res->isNegative = vb.isNegative;
        BigIntView rv = {r->digits, r->size, false};
        sub_abs(res, vb, rv);
        return pack_bigint(L, res);
    }
    
    return pack_bigint(L, r);
}

BigInt luaZ_bigint_rem(lua_State* L, BigInt a, BigInt b)
{
    if (!a.heap && !b.heap) {
        if (b.smi != 0) {
            if (a.smi == INT64_MIN && b.smi == -1) {
                return luaZ_newbigint(0);
            }
            return luaZ_newbigint(a.smi % b.smi);
        } else {
            luaG_runerror(L, "attempt to perform 'n%%0'");
        }
    }
    
    uint32_t ta[2], tb[2];
    BigIntView va = get_view(a, ta);
    BigIntView vb = get_view(b, tb);
    
    HeapBigInt* r = lua_newheapbigint(L, vb.size);
    for(uint32_t i=0; i < r->capacity; i++) r->digits[i] = 0; // initialize zero
    
    r->isNegative = va.isNegative; // C-style remainder follows dividend
    div_mod_abs(L, va, vb, nullptr, r);
    
    return pack_bigint(L, r);
}

BigInt luaZ_bigint_neg(lua_State* L, BigInt a)
{
    if (!a.heap) {
        if (a.smi == INT64_MIN) {
            // Overflows SMI, fallback to HeapBigInt
            HeapBigInt* res = lua_newheapbigint(L, 3);
            res->isNegative = false;
            res->size = 3;
            res->digits[0] = 0;
            res->digits[1] = 0;
            res->digits[2] = 0x80000000;
            return pack_bigint(L, res);
        }
        return luaZ_newbigint(-a.smi);
    }
    
    HeapBigInt* res = lua_newheapbigint(L, a.heap->size);
    res->size = a.heap->size;
    res->isNegative = !a.heap->isNegative;
    for (uint32_t i = 0; i < a.heap->size; ++i) {
        res->digits[i] = a.heap->digits[i];
    }
    return pack_bigint(L, res);
}

BigInt luaZ_bigint_fromstring(lua_State* L, const char* str)
{
    BigInt res = luaZ_newbigint(0);
    bool isNegative = false;
    if (*str == '-')
    {
        isNegative = true;
        str++;
    }
    else if (*str == '+')
    {
        str++;
    }

    while (*str)
    {
        if (*str >= '0' && *str <= '9')
        {
            BigInt ten = luaZ_newbigint(10);
            BigInt digit = luaZ_newbigint(*str - '0');
            res = luaZ_bigint_add(L, luaZ_bigint_mul(L, res, ten), digit);
        }
        else
        {
            break; // Malformed or ending
        }
        str++;
    }

    if (isNegative)
    {
        res = luaZ_bigint_neg(L, res);
    }
    return res;
}

void lua_pushbigint_string(lua_State* L, BigInt b)
{
    if (!b.heap)
    {
        char buf[64];
        snprintf(buf, 64, "%lld", (long long)b.smi);
        lua_pushstring(L, buf);
        return;
    }

    char buf[400];
    int pos = 0;
    
    BigInt ten = luaZ_newbigint(10);
    BigInt current = b;
    bool isNegative = current.heap->isNegative;
    
    BigInt absCurrent;
    if (isNegative) {
        absCurrent = luaZ_bigint_neg(L, current);
    } else {
        absCurrent = current;
    }
    
    while (true)
    {
        uint32_t view[2];
        BigIntView v = get_view(absCurrent, view);
        bool isZero = true;
        for (uint32_t i = 0; i < v.size; i++) {
            if (v.digits[i] != 0) { isZero = false; break; }
        }
        if (isZero) break;
        
        BigInt rem = luaZ_bigint_rem(L, absCurrent, ten);
        BigInt div = luaZ_bigint_div(L, absCurrent, ten);
        
        buf[pos++] = '0' + (char)(rem.heap ? 0 : rem.smi);
        absCurrent = div;
    }
    
    if (pos == 0) {
        buf[pos++] = '0';
    } else if (isNegative) {
        buf[pos++] = '-';
    }
    
    // Reverse the string
    for (int i = 0; i < pos / 2; i++) {
        char tmp = buf[i];
        buf[i] = buf[pos - 1 - i];
        buf[pos - 1 - i] = tmp;
    }
    
    lua_pushlstring(L, buf, pos);
}

