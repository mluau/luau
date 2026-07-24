// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lobject.h"
#include "lstate.h"
#include "lgc.h"
#include "lmem.h"
#include "luaconf.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct Integer {
    int64_t smi;
    HeapInteger* heap;
    IntegerMode mode;
};

inline Integer unpack_integer(const TValue* o) {
    Integer b;
    if (ttype(o) == LUA_TINTEGER) {
        b.smi = o->value.l;
        b.heap = nullptr;
    } else {
        b.smi = 0;
        b.heap = (HeapInteger*)o->value.gc;
    }
    b.mode = (IntegerMode)o->extra[0];
    return b;
}

inline void pack_integer(TValue* obj, Integer b) {
    if (b.heap) {
        setintegerheap(obj, b.heap, b.mode);
    } else {
        setintegersmi(obj, b.smi, b.mode);
    }
}

inline Integer new_integer(int64_t v, IntegerMode mode = IntegerMode_Dynamic) {
    Integer b;
    b.smi = v;
    b.heap = nullptr;
    b.mode = mode;
    return b;
}

inline Integer new_integer_from_heap(HeapInteger* h) {
    Integer b;
    b.smi = 0;
    b.heap = h;
    b.mode = IntegerMode_Dynamic;
    return b;
}

uint64_t internal_get_bottom_64(Integer b) {
    if (!b.heap) return (uint64_t)b.smi;
    uint64_t val = 0;
    if (b.heap->size > 0) val |= b.heap->digits[0];
    if (b.heap->size > 1) val |= ((uint64_t)b.heap->digits[1] << 32);
    if (b.heap->isNegative) val = ~val + 1;
    return val;
}

uint64_t luaZ_integer_get_bottom_64(const TValue* o) {
    return internal_get_bottom_64(unpack_integer(o));
}

#define HANDLE_TYPED_MATH(L, a, b, OP) \
    if (a.mode != IntegerMode_Dynamic || b.mode != IntegerMode_Dynamic) { \
        if (a.mode != b.mode) { \
            luaG_runerror(L, "attempt to perform arithmetic on mixed typed integers"); \
        } \
        IntegerMode mode = a.mode; \
        uint64_t va = internal_get_bottom_64(a); \
        uint64_t vb = internal_get_bottom_64(b); \
        uint64_t raw_res = va OP vb; \
        uint8_t shift = luau_int_shifts[mode]; \
        uint64_t res = 0; \
        if (luau_int_signed[mode]) \
            res = (int64_t)(raw_res << shift) >> shift; \
        else \
            res = (raw_res << shift) >> shift; \
        return new_integer((int64_t)res, mode); \
    }

#define DO_TYPED_DIV(TYPE, UTYPE, OP) { UTYPE ta = (UTYPE)va, tb = (UTYPE)vb; if (tb == (UTYPE)-1 && ta == (UTYPE)((TYPE)1 << (sizeof(TYPE)*8-1))) { res = (int64_t)(TYPE)ta; } else { res = (int64_t)(TYPE)((TYPE)ta OP (TYPE)tb); } }
#define DO_TYPED_UDIV(UTYPE, OP) { res = (uint64_t)(UTYPE)((UTYPE)va OP (UTYPE)vb); }

#define HANDLE_TYPED_DIV(L, a, b, OP, is_mod) \
    if (a.mode != IntegerMode_Dynamic || b.mode != IntegerMode_Dynamic) { \
        if (a.mode != b.mode) { \
            luaG_runerror(L, "attempt to perform arithmetic on mixed typed integers"); \
        } \
        IntegerMode mode = a.mode; \
        uint64_t va = internal_get_bottom_64(a); \
        uint64_t vb = internal_get_bottom_64(b); \
        if (vb == 0) luaG_runerror(L, is_mod ? "attempt to perform modulo by zero" : "attempt to divide by zero"); \
        uint64_t res = 0; \
        switch (mode) { \
            case IntegerMode_I8: DO_TYPED_DIV(int8_t, uint8_t, OP); break; \
            case IntegerMode_U8: DO_TYPED_UDIV(uint8_t, OP); break; \
            case IntegerMode_I16: DO_TYPED_DIV(int16_t, uint16_t, OP); break; \
            case IntegerMode_U16: DO_TYPED_UDIV(uint16_t, OP); break; \
            case IntegerMode_I32: DO_TYPED_DIV(int32_t, uint32_t, OP); break; \
            case IntegerMode_U32: DO_TYPED_UDIV(uint32_t, OP); break; \
            case IntegerMode_I64: DO_TYPED_DIV(int64_t, uint64_t, OP); break; \
            case IntegerMode_U64: DO_TYPED_UDIV(uint64_t, OP); break; \
            default: break; \
        } \
        return new_integer((int64_t)res, mode); \
    }


static HeapInteger* lua_newheapinteger(lua_State* L, uint32_t capacity)
{
    HeapInteger* h = luaM_newgco(L, HeapInteger, sizeof(HeapInteger), L->activememcat);
    luaC_init(L, h, LUA_THEAPINTEGER);
    h->capacity = capacity;
    h->size = 0;
    h->isNegative = false;
    if (capacity > 0)
        h->digits = luaM_newarray(L, capacity, uint32_t, L->activememcat);
    else
        h->digits = nullptr;
    return h;
}

void lua_freeinteger(lua_State* L, HeapInteger* h, struct lua_Page* page)
{
    if (h->capacity > 0)
        luaM_freearray(L, h->digits, h->capacity, uint32_t, h->memcat);
    luaM_freegco(L, h, sizeof(HeapInteger), h->memcat, page);
}

struct IntegerView {
    const uint32_t* digits;
    uint32_t size;
    bool isNegative;
};

static IntegerView get_view(const Integer& b, uint32_t temp[2]) {
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

bool luaZ_integer_eq(const TValue* a_val, const TValue* b_val)
{
    if (a_val->extra[0] != b_val->extra[0]) return false;
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);

    if (va.size != vb.size) return false;
    if (va.size == 0) return true;
    if (va.isNegative != vb.isNegative) return false;
    return memcmp(va.digits, vb.digits, va.size * sizeof(uint32_t)) == 0;
}

bool luaZ_integer_eq_key(const TKey* a_key, const TValue* b_val)
{
    if (a_key->extra[0] != b_val->extra[0]) return false;
    Integer a;
    if (a_key->tt == LUA_TINTEGER)
    {
        a.smi = a_key->value.l;
        a.heap = nullptr;
    }
    else // LUA_THEAPINTEGER
    {
        a.smi = 0;
        a.heap = (HeapInteger*)a_key->value.gc;
    }
    a.mode = (IntegerMode)a_key->extra[0];

    Integer b = unpack_integer(b_val);
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);

    if (va.size != vb.size) return false;
    if (va.size == 0) return true;
    if (va.isNegative != vb.isNegative) return false;
    return memcmp(va.digits, vb.digits, va.size * sizeof(uint32_t)) == 0;
}

uint32_t luaZ_integer_hash(const TValue* b_val)
{
    Integer b = unpack_integer(b_val);
    uint32_t t[2];
    IntegerView v = get_view(b, t);
    
    uint32_t mode = b_val->extra[0];
    uint32_t h = v.size ^ (v.isNegative ? 0x80000000 : 0) ^ (mode << 16);
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


static void normalize(HeapInteger* h) {
    while (h->size > 0 && h->digits[h->size - 1] == 0) {
        h->size--;
    }
    if (h->size == 0) {
        h->isNegative = false;
    }
}

static Integer pack_integer_impl(lua_State* L, HeapInteger* h) {
    normalize(h);
    if (h->size == 0) {
        return new_integer(0);
    }
    if (h->size <= 2) {
        uint64_t val = h->digits[0];
        if (h->size == 2) {
            val |= ((uint64_t)h->digits[1] << 32);
        }
        if (!h->isNegative && val <= (uint64_t)INT64_MAX) {
            return new_integer((int64_t)val);
        } else if (h->isNegative && val <= (uint64_t)INT64_MAX + 1) {
            return new_integer(val == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)val);
        }
    }
    return new_integer_from_heap(h);
}



static int cmp_abs(const IntegerView& a, const IntegerView& b) {
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

static void add_abs(HeapInteger* res, const IntegerView& a, const IntegerView& b) {
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

static void sub_abs(HeapInteger* res, const IntegerView& a, const IntegerView& b) {
    // Requires a >= b
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < a.size; ++i) {
        uint64_t diff = a.digits[i] - borrow;
        if (i < b.size) diff -= b.digits[i];
        res->digits[res->size++] = (uint32_t)(diff & 0xFFFFFFFF);
        borrow = (diff >> 32) ? 1 : 0;
    }
}

static void mul_abs(HeapInteger* res, const IntegerView& a, const IntegerView& b) {
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

static void div_mod_abs(lua_State* L, const IntegerView& n, const IntegerView& d, HeapInteger* q, HeapInteger* r) {
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
    HeapInteger* rem = lua_newheapinteger(L, n.size);
    rem->size = n.size;
    for(uint32_t i = 0; i < n.size; i++) rem->digits[i] = n.digits[i];

    HeapInteger* shift_d = lua_newheapinteger(L, n.size + 1);
    
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

        IntegerView r_view = {rem->digits, rem->size, false};
        IntegerView sd_view = {shift_d->digits, shift_d->size, false};
        
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

static Integer integer_add_impl(lua_State* L, Integer a, Integer b)
{
    HANDLE_TYPED_MATH(L, a, b, +)
    if (!a.heap && !b.heap) {
        int64_t sum;
        if (!__builtin_add_overflow(a.smi, b.smi, &sum))
            return new_integer(sum);
    }
    
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);
    
    HeapInteger* res = lua_newheapinteger(L, (va.size > vb.size ? va.size : vb.size) + 1);
    
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
    return pack_integer_impl(L, res);
}

static Integer integer_sub_impl(lua_State* L, Integer a, Integer b)
{
    HANDLE_TYPED_MATH(L, a, b, -)
    if (!a.heap && !b.heap) {
        int64_t diff;
        if (!__builtin_sub_overflow(a.smi, b.smi, &diff))
            return new_integer(diff);
    }
    
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);
    
    HeapInteger* res = lua_newheapinteger(L, (va.size > vb.size ? va.size : vb.size) + 1);
    
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
    return pack_integer_impl(L, res);
}

static Integer integer_mul_impl(lua_State* L, Integer a, Integer b)
{
    HANDLE_TYPED_MATH(L, a, b, *)
    if (!a.heap && !b.heap) {
        int64_t prod;
        if (!__builtin_mul_overflow(a.smi, b.smi, &prod))
            return new_integer(prod);
    }
    
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);
    
    HeapInteger* res = lua_newheapinteger(L, va.size + vb.size);
    for(uint32_t i=0; i < res->capacity; i++) res->digits[i] = 0; // initialize zero
    
    res->isNegative = va.isNegative != vb.isNegative;
    mul_abs(res, va, vb);
    
    return pack_integer_impl(L, res);
}

static Integer integer_div_impl(lua_State* L, Integer a, Integer b)
{
    HANDLE_TYPED_DIV(L, a, b, /, false)
    if (!a.heap && !b.heap) {
        if (b.smi != 0) {
            // Check for INT64_MIN / -1
            if (a.smi == INT64_MIN && b.smi == -1) {
                // Will overflow int64, fallback to heap
            } else {
                return new_integer(a.smi / b.smi);
            }
        } else {
            luaG_runerror(L, "attempt to divide by zero");
        }
    }
    
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);
    
    HeapInteger* q = lua_newheapinteger(L, va.size);
    for(uint32_t i=0; i < q->capacity; i++) q->digits[i] = 0; // initialize zero
    
    q->isNegative = va.isNegative != vb.isNegative;
    
    div_mod_abs(L, va, vb, q, nullptr);
    
    return pack_integer_impl(L, q);
}

static Integer integer_mod_impl(lua_State* L, Integer a, Integer b)
{
    HANDLE_TYPED_DIV(L, a, b, %, true)
    if (!a.heap && !b.heap) {
        if (b.smi != 0) {
            if (a.smi == INT64_MIN && b.smi == -1) {
                return new_integer(0);
            }
            int64_t r = a.smi % b.smi;
            if (r != 0 && (a.smi ^ b.smi) < 0) {
                r += b.smi;
            }
            return new_integer(r);
        } else {
            luaG_runerror(L, "attempt to perform 'n%%0'");
        }
    }
    
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);
    
    HeapInteger* r = lua_newheapinteger(L, vb.size);
    for(uint32_t i=0; i < r->capacity; i++) r->digits[i] = 0; // initialize zero
    
    r->isNegative = va.isNegative;
    div_mod_abs(L, va, vb, nullptr, r);
    normalize(r);

    if (r->size > 0 && va.isNegative != vb.isNegative) {
        // Lua modulo: r = r + b when signs differ and r != 0
        HeapInteger* res = lua_newheapinteger(L, (r->size > vb.size ? r->size : vb.size) + 1);
        res->isNegative = vb.isNegative;
        IntegerView rv = {r->digits, r->size, false};
        sub_abs(res, vb, rv);
        return pack_integer_impl(L, res);
    }
    
    return pack_integer_impl(L, r);
}

static Integer integer_rem_impl(lua_State* L, Integer a, Integer b)
{
    HANDLE_TYPED_DIV(L, a, b, %, true)
    if (!a.heap && !b.heap) {
        if (b.smi != 0) {
            if (a.smi == INT64_MIN && b.smi == -1) {
                return new_integer(0);
            }
            return new_integer(a.smi % b.smi);
        } else {
            luaG_runerror(L, "attempt to perform 'n%%0'");
        }
    }
    
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);
    
    HeapInteger* r = lua_newheapinteger(L, vb.size);
    for(uint32_t i=0; i < r->capacity; i++) r->digits[i] = 0; // initialize zero
    
    r->isNegative = va.isNegative; // C-style remainder follows dividend
    div_mod_abs(L, va, vb, nullptr, r);
    
    return pack_integer_impl(L, r);
}

static Integer integer_neg_impl(lua_State* L, Integer a)
{
    if (!a.heap) {
        if (a.mode != IntegerMode_Dynamic) {
            uint64_t va = internal_get_bottom_64(a);
            uint64_t res = 0;
            switch (a.mode) {
                case IntegerMode_I8: res = (int64_t)(int8_t)-(int8_t)va; break;
                case IntegerMode_U8: res = (uint64_t)(uint8_t)-(uint8_t)va; break;
                case IntegerMode_I16: res = (int64_t)(int16_t)-(int16_t)va; break;
                case IntegerMode_U16: res = (uint64_t)(uint16_t)-(uint16_t)va; break;
                case IntegerMode_I32: res = (int64_t)(int32_t)-(int32_t)va; break;
                case IntegerMode_U32: res = (uint64_t)(uint32_t)-(uint32_t)va; break;
                case IntegerMode_I64: res = (int64_t)(int64_t)-(int64_t)va; break;
                case IntegerMode_U64: res = (uint64_t)(uint64_t)-(uint64_t)va; break;
                default: break;
            }
            return new_integer((int64_t)res, a.mode);
        }
        
        if (a.smi == INT64_MIN) {
            // Overflows SMI, fallback to HeapInteger
            HeapInteger* res = lua_newheapinteger(L, 3);
            res->isNegative = false;
            res->size = 3;
            res->digits[0] = 0;
            res->digits[1] = 0;
            res->digits[2] = 0x80000000;
            return pack_integer_impl(L, res);
        }
        return new_integer(-a.smi);
    }
    
    HeapInteger* res = lua_newheapinteger(L, a.heap->size);
    res->size = a.heap->size;
    res->isNegative = !a.heap->isNegative;
    for (uint32_t i = 0; i < a.heap->size; ++i) {
        res->digits[i] = a.heap->digits[i];
    }
    return pack_integer_impl(L, res);
}

static Integer integer_fromstring_impl(lua_State* L, const char* str)
{
    Integer res = new_integer(0);
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
            Integer ten = new_integer(10);
            Integer digit = new_integer(*str - '0');
            res = integer_add_impl(L, integer_mul_impl(L, res, ten), digit);
        }
        else
        {
            break; // Malformed or ending
        }
        str++;
    }

    if (isNegative)
    {
        res = integer_neg_impl(L, res);
    }
    return res;
}

static void integer_push_string_impl(lua_State* L, Integer b)
{
    if (!b.heap)
    {
        char buf[64];
        if (b.mode == IntegerMode_U8 || b.mode == IntegerMode_U16 || b.mode == IntegerMode_U32 || b.mode == IntegerMode_U64) {
            snprintf(buf, 64, "%llu", (unsigned long long)b.smi);
        } else {
            snprintf(buf, 64, "%lld", (long long)b.smi);
        }
        lua_pushstring(L, buf);
        return;
    }

    char buf[400];
    int pos = 0;
    
    Integer ten = new_integer(10);
    Integer current = b;
    bool isNegative = current.heap->isNegative;
    
    Integer absCurrent;
    if (isNegative) {
        absCurrent = integer_neg_impl(L, current);
    } else {
        absCurrent = current;
    }
    
    while (true)
    {
        uint32_t view[2];
        IntegerView v = get_view(absCurrent, view);
        bool isZero = true;
        for (uint32_t i = 0; i < v.size; i++) {
            if (v.digits[i] != 0) { isZero = false; break; }
        }
        if (isZero) break;
        
        Integer rem = integer_rem_impl(L, absCurrent, ten);
        Integer div = integer_div_impl(L, absCurrent, ten);
        
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
void luaZ_integer_add(lua_State* L, const TValue* a_val, const TValue* b_val, TValue* res_out) {
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    pack_integer(res_out, integer_add_impl(L, a, b));
}

void luaZ_integer_sub(lua_State* L, const TValue* a_val, const TValue* b_val, TValue* res_out) {
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    pack_integer(res_out, integer_sub_impl(L, a, b));
}

void luaZ_integer_mul(lua_State* L, const TValue* a_val, const TValue* b_val, TValue* res_out) {
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    pack_integer(res_out, integer_mul_impl(L, a, b));
}

void luaZ_integer_div(lua_State* L, const TValue* a_val, const TValue* b_val, TValue* res_out) {
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    pack_integer(res_out, integer_div_impl(L, a, b));
}

void luaZ_integer_mod(lua_State* L, const TValue* a_val, const TValue* b_val, TValue* res_out) {
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    pack_integer(res_out, integer_mod_impl(L, a, b));
}

void luaZ_integer_rem(lua_State* L, const TValue* a_val, const TValue* b_val, TValue* res_out) {
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    pack_integer(res_out, integer_rem_impl(L, a, b));
}

void luaZ_integer_neg(lua_State* L, const TValue* a_val, TValue* res_out) {
    Integer a = unpack_integer(a_val);
    pack_integer(res_out, integer_neg_impl(L, a));
}

void luaZ_integer_fromstring(lua_State* L, const char* str, TValue* res_out) {
    pack_integer(res_out, integer_fromstring_impl(L, str));
}

void lua_pushinteger_string(lua_State* L, const TValue* b_val) {
    Integer b = unpack_integer(b_val);
    integer_push_string_impl(L, b);
}

bool luaZ_integer_lt(lua_State* L, const TValue* a_val, const TValue* b_val)
{
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    if (a.mode != b.mode)
        luaG_runerror(L, "attempt to compare mixed typed integers");
        
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);
    
    if (va.size == 0 && vb.size == 0) return false;
    if (va.isNegative && !vb.isNegative) return true;
    if (!va.isNegative && vb.isNegative) return false;
    
    int cmp = cmp_abs(va, vb);
    if (va.isNegative)
        return cmp > 0;
    else
        return cmp < 0;
}

bool luaZ_integer_le(lua_State* L, const TValue* a_val, const TValue* b_val)
{
    Integer a = unpack_integer(a_val);
    Integer b = unpack_integer(b_val);
    if (a.mode != b.mode)
        luaG_runerror(L, "attempt to compare mixed typed integers");
        
    uint32_t ta[2], tb[2];
    IntegerView va = get_view(a, ta);
    IntegerView vb = get_view(b, tb);
    
    if (va.size == 0 && vb.size == 0) return true;
    if (va.isNegative && !vb.isNegative) return true;
    if (!va.isNegative && vb.isNegative) return false;
    
    int cmp = cmp_abs(va, vb);
    if (va.isNegative)
        return cmp >= 0;
    else
        return cmp <= 0;
}
