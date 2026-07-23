# Arbitrary-Precision BigInt

Status: Draft

## Summary

Introduce an arbitrary-precision `bigint` type to Luau. This new type handles large integers while optimizing performance by storing values <= 64 bits inline within the VM's value representation. 

## Motivation

Luau numbers are IEEE 754 doubles, which lose precision beyond `2^53 - 1`. There are many use cases for strict 64-bit precision (e.g., bitfields, Discord permissions) or larger numbers (e.g., simulations). Introducing a `bigint` type solves this. This RFC also replaces the upstream `FFlag::LuauIntegerType2` `int64` implementation, which lacks metatable support and other features.

## Design

### Object Representation & SMI

The upstream `LUA_TINTEGER` type will be removed and replaced with `LUA_TBIGINT`. 

To maintain performance, `bigint` values use a **Small Integer (SMI)** optimization:

* **Inline (<= 64-bit):** Fits within a 64-bit payload, stored directly inline within the VM's value.
* **Heap (Overflow):** Values dynamically promote to a heap-allocated, garbage-collected object containing a sign flag and an array of 32-bit digits (similar to V8's BigInt representation).

**Typed Modes:**
The `TValue` structural `extra[0]` payload is utilized to mark explicitly typed `bigint` modes with zero memory overhead. A `BigIntMode` enum distinguishes between untyped/dynamic BigInts (which can grow to the heap) and explicitly typed bounds (`u8`, `i8`, `u16`, `i16`, `u32`, `i32`, `u64`, `i64`). 

By default, mathematical operations on typed bigints **wrap on overflow** to ensure maximum type safety. 

**Mode Propagation Rules:**
When performing binary operations (e.g., `+`, `-`, `*`) on bigints:
1. **Same Typed Modes (e.g. `u16 + u16`)**: Operations are performed explicitly within that bounded type and return a bigint of the same typed mode.
2. **Mixed Typed Modes (e.g. `u16 + u32`, `u32 + dynamic`)**: Throws a runtime type error requiring the user to explicitly cast them to identical types. This strictly enforces type safety.

### Arithmetic & Relational Operations

A compliant implementation must support the following core operations on `bigint` values:

* `bigint + bigint -> bigint`: Addition
* `bigint - bigint -> bigint`: Subtraction
* `bigint * bigint -> bigint`: Multiplication
* `bigint / bigint -> bigint`: Division
* `bigint % bigint -> bigint`: Modulo
* `-bigint -> bigint`: Negation
* `bigint < bigint -> boolean`: Less than
* `bigint <= bigint -> boolean`: Less than or equal
* `bigint > bigint -> boolean`: Greater than
* `bigint >= bigint -> boolean`: Greater than or equal

**Fast-Path:** If both operands are inline 64-bit integers, the VM performs fast hardware math, checking for overflow.

**Slow-Path:** If an overflow occurs, or an operand is heap-allocated, it dispatches to a new arbitrary-precision library.

### Native Code Generation (NCG)

NCG will only emit optimized fast-paths for inline 64-bit integers. Non-int64 cases or overflows will safely fall back to the VM's slow path.

### Language & Standard Library

* **Literals:** Constructed using the `i` suffix (e.g., `123i`), maintaining compatibility with `FFlag::LuauIntegerType2`.
* **Mixed Math:** Mixing `bigint` and `number` (e.g., `123i + 1.5`) throws a runtime type error to prevent precision loss.
* **Type System:** A `bigint` primitive type will be added to the typechecker.
* **Type & String Conversion:** Calling `type()` on a bigint returns `"bigint"`. Calling `tostring()` returns the string representation of the integer without any suffix (e.g., `"123"`, not `"123i"`).
### BigInt Library

A `bigint` standard library will be provided with the following methods:

`function bigint.dynamic(n: bigint): bigint`

Coerces a typed bigint back into a standard untyped/arbitrary-precision bigint.

- `bigint.i8(n)`: Bounds as signed 8-bit integer.
- `bigint.u8(n)`: Bounds as unsigned 8-bit integer.
- `bigint.i16(n)`: Bounds as signed 16-bit integer.
- `bigint.u16(n)`: Bounds as unsigned 16-bit integer.
- `bigint.i32(n)`: Bounds as signed 32-bit integer.
- `bigint.u32(n)`: Same as above but bounds as unsigned 32-bit integer.
- `bigint.i64(n)`: Same as above but bounds as signed 64-bit integer.
- `bigint.u64(n)`: Same as above but bounds as unsigned 64-bit integer.
- `bigint.wi8(n)`, `bigint.wu8(n)`, `bigint.wi16(n)`, `bigint.wu16(n)`, `bigint.wi32(n)`, `bigint.wu32(n)`, `bigint.wi64(n)`, `bigint.wu64(n)`: Same as the respective bounds, but mathematical operations on these types will implicitly wrap instead of throwing an overflow error.

`function bigint.min(...: bigint): bigint`

Returns the minimum value among the arguments.

`function bigint.max(...: bigint): bigint`

Returns the maximum value among the arguments.

`function bigint.rem(a: bigint, b: bigint): bigint`

Returns the remainder of dividing `a` by `b`.

`function bigint.clamp(n: bigint, min: bigint, max: bigint): bigint`

Returns `n` clamped between `min` and `max`.

`function bigint.bnot(n: bigint): bigint`

Returns the bitwise NOT of `n`.

`function bigint.band(...: bigint): bigint`

Returns the bitwise AND of the arguments.

`function bigint.bor(...: bigint): bigint`

Returns the bitwise OR of the arguments.

`function bigint.bxor(...: bigint): bigint`

Returns the bitwise XOR of the arguments.

`function bigint.lshift(n: bigint, i: bigint): bigint`

Returns `n` logically shifted left by `i` bits.

`function bigint.rshift(n: bigint, i: bigint): bigint`

Returns `n` logically shifted right by `i` bits.

`function bigint.arshift(n: bigint, i: bigint): bigint`

Returns `n` arithmetically shifted right by `i` bits.

`function bigint.lrotate(n: bigint, i: bigint): bigint`

Rotates `n` to the left by `i` bits (if `i` is negative, a right rotate is performed instead).

`function bigint.rrotate(n: bigint, i: bigint): bigint`

Rotates `n` to the right by `i` bits (if `i` is negative, a left rotate is performed instead).

`function bigint.wadd(a: bigint, b: bigint): bigint`

Returns the wrapping addition of `a` and `b`.

`function bigint.sadd(a: bigint, b: bigint, max: bigint): bigint`

Returns the saturating addition of `a` and `b` up to `max`.

## Drawbacks

* **VM Complexity:** Adding branch checks for `LUA_TBIGINT` in the execution loop slightly increases complexity.
* **Maintenance:** A custom arbitrary-precision bignum library requires careful testing and long-term maintenance.

## Alternatives

* **Userdata Wrapping:** Host applications could implement BigInts via `userdata`, but this suffers from performance degradation, forces heap allocations for all math, and lacks typechecker support.
