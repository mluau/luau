# Arbitrary-Precision Integer

Status: Draft

## Summary

Introduce an arbitrary-precision `integer` type to Luau. This new type handles large integers while optimizing performance by storing values <= 64 bits inline within the VM's value representation. 

## Motivation

Luau numbers are IEEE 754 doubles, which lose precision beyond `2^53 - 1`. There are many use cases for strict 64-bit precision (e.g., bitfields, Discord permissions) or larger numbers (e.g., simulations). Introducing a `integer` type solves this. This RFC also replaces the upstream `FFlag::LuauIntegerType2` `int64` implementation, which lacks metatable support and other features.

## Design

### Object Representation & SMI

The upstream `LUA_TINTEGER` type will be removed and replaced with `LUA_TINTEGER`. `LUA_THEAPINTEGER` will be added for the arbitrary precision case. 

To maintain performance, `integer` values use a **Small Integer (SMI)** optimization:

- **Inline (<= 64-bit):** Fits within a 64-bit payload, stored directly inline within the VM's value.
- **Heap (Overflow):** Values dynamically promote to a heap-allocated, garbage-collected object containing a sign flag and an array of 32-bit digits (similar to V8's Integer representation).

**Typed Modes:**
The `TValue` structural `extra[0]` payload is utilized to mark explicitly typed `integer` modes with zero memory overhead. A `IntegerMode` enum distinguishes between untyped/dynamic integers (which can be promoted to the heap) and explicitly typed bounds (`u8`, `i8`, `u16`, `i16`, `u32`, `i32`, `u64`, `i64`). 

By default, mathematical operations on typed integers **wrap on overflow** to ensure type safety while also maintaining performance. Operations can only be performed on typed integers of the same type (casting between integer subtypes is allowed and also wraps).

### Arithmetic & Relational Operations

A compliant implementation must support the following core operations on `integer` values:

- `integer + integer -> integer`: Addition
- `integer - integer -> integer`: Subtraction
- `integer * integer -> integer`: Multiplication
- `integer / integer -> integer`: Division
- `integer % integer -> integer`: Modulo
- `-integer -> integer`: Negation
- `integer < integer -> boolean`: Less than
- `integer <= integer -> boolean`: Less than or equal
- `integer > integer -> boolean`: Greater than
- `integer >= integer -> boolean`: Greater than or equal

### Language & Standard Library

- **Literals:** Constructed using either typed suffixes (`u8` through `u64` and `i8` through `i64`) or the `n` suffix (e.g., `123i`) for dynamic/untyped integers. For maintaining compatibility with upstream Luau, the `i` suffix is also supported and subtypes to `i64`
- **Mixed Math:** Mixing `integer` and `number` (e.g., `123i + 1.5`) throws a runtime type error. An `integer` will also never equal a `number`
- **Type System:** A `integer` primitive type will be added to the typechecker along with the subtyped integers as dedicated nominal types
- **Type & String Conversion:** Calling `type()` on a integer returns `"integer"`. Calling `typeof()` on an integer returns either the subtype of the integer or `integer` for untyped integers. Calling `tostring()` returns the string representation of the integer without any suffix (e.g., `"123"`, not `"123i"`).

### Integer Library

A `integer` standard library will be provided with the following methods:

`function integer.dynamic(n: integer): integer`

Coerces a typed integer back into a standard untyped/arbitrary-precision integer.

- `integer.i8(n)`: Bounds as signed 8-bit integer.
- `integer.u8(n)`: Bounds as unsigned 8-bit integer.
- `integer.i16(n)`: Bounds as signed 16-bit integer.
- `integer.u16(n)`: Bounds as unsigned 16-bit integer.
- `integer.i32(n)`: Bounds as signed 32-bit integer.
- `integer.u32(n)`: Same as above but bounds as unsigned 32-bit integer.
- `integer.i64(n)`: Same as above but bounds as signed 64-bit integer.
- `integer.u64(n)`: Same as above but bounds as unsigned 64-bit integer.

`function integer.min(...: integer): integer`

Returns the minimum value among the arguments.

`function integer.max(...: integer): integer`

Returns the maximum value among the arguments.

`function integer.rem(a: integer, b: integer): integer`

Returns the remainder of dividing `a` by `b`.

`function integer.clamp(n: integer, min: integer, max: integer): integer`

Returns `n` clamped between `min` and `max`.

`function integer.bnot(n: integer): integer`

Returns the bitwise NOT of `n`.

`function integer.band(...: integer): integer`

Returns the bitwise AND of the arguments.

`function integer.bor(...: integer): integer`

Returns the bitwise OR of the arguments.

`function integer.bxor(...: integer): integer`

Returns the bitwise XOR of the arguments.

`function integer.lshift(n: integer, i: integer): integer`

Returns `n` logically shifted left by `i` bits.

`function integer.rshift(n: integer, i: integer): integer`

Returns `n` logically shifted right by `i` bits.

`function integer.arshift(n: integer, i: integer): integer`

Returns `n` arithmetically shifted right by `i` bits.

`function integer.lrotate(n: integer, i: integer): integer`

Rotates `n` to the left by `i` bits (if `i` is negative, a right rotate is performed instead).

`function integer.rrotate(n: integer, i: integer): integer`

Rotates `n` to the right by `i` bits (if `i` is negative, a left rotate is performed instead).

`function integer.wadd(a: integer, b: integer): integer`

Returns the wrapping addition of `a` and `b`.

`function integer.sadd(a: integer, b: integer, max: integer): integer`

Returns the saturating addition of `a` and `b` up to `max`.

## Drawbacks

- **VM Complexity:** Adding branch checks for `LUA_TINTEGER` in the execution loop slightly increases complexity.
- **Maintenance:** A custom arbitrary-precision bignum library requires careful testing and long-term maintenance.

## Alternatives

- **Userdata Wrapping:** Host applications could implement Integers via `userdata`, but this suffers from performance degradation, forces heap allocations for all math, and lacks typechecker support.
