# External Buffers

Status: Implemented

## Summary

Add support for external buffers to the Luau VM, allowing buffers to wrap existing host-managed memory allocations. Introduce a new C API for creating these buffers and a `buffer.isfrozen` standard library function to query their mutability.

## Motivation

Luau's `buffer` type provides a way to represent fixed-size, mutable byte arrays. However, memory for standard buffers is always allocated and managed by the Luau VM itself. In many embedding scenarios (such as custom Luau runtimes), it is necessary to expose pre-existing chunks of memory (e.g., database files, images, file contents) to Luau scripts without the overhead of copying the data into a new Luau-allocated buffer. 


By introducing external buffers, we allow embeddings to wrap existing memory allocations without copying, restrict mutation from scripts if necessary, and allow the Luau garbage collector to accurately track the memory footprint of externally allocated data.

### Immutability

Host applications often require strict immutability guarantees for shared memory regions. Enforced read-only access enables safe, zero-copy integration where externally managed data, such as reference-counted structures, can be shared immutably between the host and VM.

## Design

### C API Additions

The Luau C API is expanded with the following functions and types:

    #define LUA_BLUAU 0
    #define LUA_BHOST_IMMUTABLE 1
    #define LUA_BHOST_MUTABLE 2

    typedef void (*lua_BufferFree)(lua_State* L, void* data, size_t sz, void* userdata);
    LUA_API void* lua_newexternalbuffer(
        lua_State* L, 
        size_t sz, 
        void* data, 
        void* userdata, 
        lua_BufferFree free_cb, 
        int mode
    );
    LUA_API int lua_getbuffermode(lua_State* L, int idx);
    LUA_API void* lua_getbufferuserdata(lua_State* L, int idx);

* `lua_newexternalbuffer` creates a new buffer that wraps the `data` pointer of length `sz`. 
* `userdata` is an opaque pointer that will be passed to `free_cb` alongside the buffer information.
* `free_cb` is an optional callback invoked when the buffer object is garbage collected.
* `mode` **must** be `LUA_BHOST_MUTABLE` or `LUA_BHOST_IMMUTABLE`. 
* Standard buffers allocated via `lua_newbuffer` implicitly use `LUA_BLUAU` as their mode. `LUA_BLUAU` cannot be passed to `lua_newexternalbuffer`.
* `lua_getbuffermode` returns the mode of the buffer at the given index.
* `lua_getbufferuserdata` returns the opaque `userdata` pointer associated with the buffer, or `NULL` if the buffer is a standard Luau buffer. This allows host applications to retrieve their underlying resource structures from buffer objects passed back into C/C++ from Lua, enabling operations like downcasting a buffer back to the original host resource for native manipulation.

**C API Amendment on Aug 14**

Ext buffers are now tagged for safety purposes so:

```
void* lua_newexternalbuffer(lua_State* L, size_t sz, void* data, void* userdata, int tag, int mode)
```

- `lua_newexternalbuffer` now takes `int tag` instead of `lua_BufferFree free_cb`
- `lua_BufferFree lua_getbufferdtor(lua_State* L, int tag)` and `void lua_setbufferdtor(lua_State* L, int tag, lua_BufferFree dtor)` can be used to set buffer dtor
- `int lua_buffertag(lua_State* L, int idx)` can be used to get the tag of buffer at `idx` or `-1` if untagged.


### Lua Standard Library Additions

A new function `buffer.isfrozen(b)` is added to the `buffer` standard library.

    buffer.isfrozen(b: buffer): boolean
This function returns `true` if the buffer is an external buffer created with the `LUA_BHOST_IMMUTABLE` mode, and `false` otherwise. If a buffer is immutable (frozen), any attempt to write to the buffer (e.g. `buffer.write*`) will result in a runtime error.

## Drawbacks

* **Complexity:** Introduces branching into the buffer writing fast-paths, and requires new native code generation (NCG) guards like `CHECK_BUFFER_MUTABLE`, to check if a buffer is frozen (immutable).

## Alternatives

* **Copying:** Continue to require all host data to be copied into standard Luau buffers. This is safe but incurs unacceptable performance and memory overhead for large or frequently accessed datasets.
* **Userdata:** Expose host data through a `userdata`. This lacks integration with the standard `buffer` library (e.g. `buffer.read*32` fast-paths) and `bit32` operations, and suffers from poor performance as well as a lack of proper GC memory footprint tracking.
