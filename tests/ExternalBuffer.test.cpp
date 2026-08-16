#include "lua.h"
#include "lualib.h"
#include "luacodegen.h"
#include "luacode.h"

#include "doctest.h"
#include "ScopedFlags.h"

#include <memory>
#include <string.h>

LUAU_FASTFLAG(LuauExternallyManagedBuffers)
LUAU_FASTFLAG(LuauBufferIsFrozen)

static int s_externalBufferFreeCount = 0;
static void test_buffer_free_cb(lua_State* L, void* data, size_t sz, void* userdata)
{
    s_externalBufferFreeCount++;
}

static int dostring(lua_State* L, const char* code)
{
    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(code, strlen(code), NULL, &bytecodeSize);
    if (!bytecode) return -1;
    int result = luau_load(L, "chunk", bytecode, bytecodeSize, 0);
    free(bytecode);
    if (result == 0)
    {
        return lua_pcall(L, 0, 0, 0);
    }
    return result;
}

static int dostring_ncg(lua_State* L, const char* code)
{
    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(code, strlen(code), NULL, &bytecodeSize);
    if (!bytecode) return -1;
    int result = luau_load(L, "chunk", bytecode, bytecodeSize, 0);
    free(bytecode);
    if (result == 0)
    {
        luau_codegen_compile(L, -1);
        return lua_pcall(L, 0, 0, 0);
    }
    return result;
}

TEST_SUITE_BEGIN("ExternalBuffers");

static int too_big_external_buffer_cb(lua_State* L)
{
    size_t len = (1 << 30) + 1; // MAX_BUFFER_SIZE + 1
    int test_userdata = 42;
    lua_setbufferdtor(L, 2, test_buffer_free_cb);
    lua_newexternalbuffer(L, len, nullptr, &test_userdata, 2, LUA_BHOST_MUTABLE);
    return 0;
}

TEST_CASE("ExternalBufferTooBig")
{
    ScopedFastFlag sff{FFlag::LuauExternallyManagedBuffers, true};
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    
    s_externalBufferFreeCount = 0;
    
    int result = lua_cpcall(L, too_big_external_buffer_cb, NULL);
    CHECK(result == LUA_ERRRUN);
    CHECK(s_externalBufferFreeCount == 1);
}


TEST_CASE("ExternalBufferMutable")
{
    ScopedFastFlag sff{FFlag::LuauExternallyManagedBuffers, true};
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    s_externalBufferFreeCount = 0;
    
    char my_memory[32];
    memset(my_memory, 0, sizeof(my_memory));

    lua_setbufferdtor(L, 3, test_buffer_free_cb);
    lua_newexternalbuffer(L, sizeof(my_memory), my_memory, nullptr, 3, LUA_BHOST_MUTABLE);
    CHECK(lua_buffertag(L, -1) == 3);
    CHECK(lua_getbuffermode(L, -1) == LUA_BHOST_MUTABLE);
    CHECK(lua_getbufferuserdata(L, -1) == nullptr);
    
    lua_setglobal(L, "ext_buf");

    // Writes should scueed as this is a mutable external buffer
    const char* lua_code = R"(
        buffer.writeu8(ext_buf, 0, 42)
        buffer.writeu8(ext_buf, 1, 100)
    )";
    
    CHECK(dostring(L, lua_code) == 0);
    CHECK(my_memory[0] == 42);
    CHECK(my_memory[1] == 100);

    // Free the state to trigger GC which should call the free_cb
    state.reset();
    CHECK(s_externalBufferFreeCount == 1);
}

TEST_CASE("ExternalBufferImmutable")
{
    ScopedFastFlag sff{FFlag::LuauExternallyManagedBuffers, true};
    ScopedFastFlag sff2{FFlag::LuauBufferIsFrozen, true};
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    s_externalBufferFreeCount = 0;
    
    char my_memory[32];
    memset(my_memory, 0, sizeof(my_memory));
    my_memory[0] = 55;

    int test_userdata = 42;

    lua_setbufferdtor(L, 2, test_buffer_free_cb);
    lua_newexternalbuffer(L, sizeof(my_memory), my_memory, &test_userdata, 2, LUA_BHOST_IMMUTABLE);
    CHECK(lua_buffertag(L, -1) == 2);
    CHECK(lua_getbuffermode(L, -1) == LUA_BHOST_IMMUTABLE);
    CHECK(lua_getbufferuserdata(L, -1) == &test_userdata);
    
    lua_setglobal(L, "ext_buf");

    // Read from buffer via Lua should succeed (just not writes)
    const char* lua_read_code = R"(
        local val = buffer.readu8(ext_buf, 0)
        assert(val == 55)
        assert(buffer.isfrozen(ext_buf) == true)
        assert(buffer.isfrozen(buffer.create(10)) == false)
    )";
    
    CHECK(dostring(L, lua_read_code) == 0);

    // Write to buffer via Lua should fail as this is a immutable buffer
    const char* lua_write_code = R"(
        buffer.writeu8(ext_buf, 0, 42)
    )";
    
    CHECK(dostring(L, lua_write_code) != 0);
    
    // Memory should remain unchanged
    CHECK(my_memory[0] == 55);

    // Free the state to trigger GC which should call the free_cb
    state.reset();
    CHECK(s_externalBufferFreeCount == 1);
}

TEST_CASE("ExternalBufferImmutable_NCG")
{
    ScopedFastFlag sff{FFlag::LuauExternallyManagedBuffers, true};
    ScopedFastFlag sff2{FFlag::LuauBufferIsFrozen, true};
    std::unique_ptr<lua_State, void (*)(lua_State*)> state(luaL_newstate(), lua_close);
    lua_State* L = state.get();
    luaL_openlibs(L);

    if (luau_codegen_supported())
    {
        luau_codegen_create(L);

        s_externalBufferFreeCount = 0;
        
        char my_memory[32];
        memset(my_memory, 0, sizeof(my_memory));
        my_memory[0] = 55;

        lua_setbufferdtor(L, 2, test_buffer_free_cb);
        lua_newexternalbuffer(L, sizeof(my_memory), my_memory, nullptr, 2, LUA_BHOST_IMMUTABLE);
        CHECK(lua_buffertag(L, -1) == 2);
        lua_setglobal(L, "ext_buf");

        // Read from buffer via Lua should succeed
        const char* lua_read_code = R"(
            local val = buffer.readu8(ext_buf, 0)
            assert(val == 55)
            assert(buffer.isfrozen(ext_buf) == true)
            assert(buffer.isfrozen(buffer.create(10)) == false)
        )";
        
        CHECK(dostring_ncg(L, lua_read_code) == 0);

        // Write to buffer via Lua should fail, EVEN WITH JIT fastcalls!
        const char* lua_write_code = R"(
            buffer.writeu8(ext_buf, 0, 42)
        )";
        
        CHECK(dostring_ncg(L, lua_write_code) != 0);
        
        // Memory should remain unchanged
        CHECK(my_memory[0] == 55);
    }
}

TEST_SUITE_END();
