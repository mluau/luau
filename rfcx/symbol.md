# RFC: First-class Symbols with Custom Truthiness

## Summary

Introduce `symbol` as a first-class primitive in Luau to provide guaranteed unique values with customizable truthiness as a generalization of `none`. Symbols will support nominal typing in the type system without needing to use classes.

## Motivation

Luau (under the `none` rfcx) now includes a primitive whole sole job is to be a falsy primitive. Symbols act as a generalization of `none` but extended. Unlike userdata (newproxy) and empty tables, symbols support custom truthiness in `if` etc. and are always unique to each other hence providing a direct generalization of `none` and potentially other primitives. By introducing symbols, we provide a lightweight, collision-free primitive with custom truthiness bounds. Additionally, symbols lay the groundwork for nominal typing in the type system outside of classes.

## Design

### The `symbol` Primitive

A symbol is a new primitive type in Luau. It is passed by value (equality is reference-based so all symbols created by `symbol.create` are unique and do not equal each other) and is immutable.

- **Equality**: Symbol equality is strictly reference-based (similar to `lightuserdata`). `symbol.create("A") ~= symbol.create("A")`.
- **Truthiness**: Each symbol carries a boolean flag determining if it evaluates to `true` or `false` in conditionals.
- **Typing**: `typeof(sym)` will return the symbol's name if one was provided, otherwise it will return `"symbol"`.

### Luau API (`symbol` library)

A new global `symbol` library will be introduced with the following API:

#### `symbol.create(name?: string, truthiness?: boolean) -> symbol`

Creates and returns a new, globally unique symbol.

- `name`: An optional string describing the symbol. If provided, `typeof(sym)` will return this name, and `tostring(sym)` will return `symbol(name, truthiness)`. If omitted, `typeof(sym)` returns `symbol` and `tostring(sym)` returns `symbol(<unnamed>, truthiness)`.
- `truthiness`: An optional boolean that dictates whether the symbol is truthy or falsy. Defaults to `true`.

*Example:*
```lua
local mysym = symbol.create("mysym", false)
print(mysym) -- "symbol(mysym)"
print(typeof(mysym)) -- "mysym"
if not mysym then
    print("This symbol is falsy!")
end

-- Guaranteed unique
assert(symbol.create("test") ~= symbol.create("test"))
```

#### `symbol.get(key: string) -> symbol`

Checks the global symbol registry for a given string `key`.

- If a symbol with this key already exists in the registry, it returns that symbol.
- If it does not exist, it creates a new symbol (using the `key` as its name), stores it in the registry, and returns it.
- Symbols created this way are interned/non-unique across calls with the same key and are also truthy.

#### `symbol.keyfor(sym: symbol) -> string | nil`

Returns the string key associated with a registry-bound symbol.

- If the symbol was created via `symbol.get(key)`, this returns `key`.
- If the symbol was created via `symbol.create(...)` (meaning it is not in the global registry), this returns `nil`.

### C API Changes

The C API will be extended to allow host applications to create and push symbols onto the stack.

#### `lua_pushsym(lua_State *L, uint8_t truthiness)`

Creates and pushes a new symbol onto the stack. If truthiness is `1`, the created symbol is truthy, otherwise it is falsy. Note the stack top must contain either a string (the name of the symbol) or `nil` (for an unnamed symbol). `lua_pushsym` consumes this value from the stack

#### `lua_pushsymnone(lua_State *L)`
Pushes the well-known, global `none` symbol onto the stack. Already exists as its own TSYMNONE type currently with the `none` rfcx already so this is just a backport/generalization step to switch `pushsymnone` to use symbol instead.

## Alternatives

- No Custom Truthiness: We could enforce that all symbols are truthy (like tables and userdata). However, this severely limits their utility as a replacement for `nil`.
- Don't add symbols and keep none: This is bad for generalizability purposes and means that Luau may end up in primitive bloat solely to satisfy properties like falsiness etc.
