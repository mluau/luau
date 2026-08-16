#include "Luau/Common.h"
#include "lua.h"
#include "lualib.h"
#include "doctest.h"
#include "ScopedFlags.h"
#include <vector>

LUAU_FASTFLAG(LuauManagedReferences2)

TEST_SUITE_BEGIN("RefPool");

TEST_CASE("RefPoolCreationAndGet")
{
    ScopedFastFlag sff{FFlag::LuauManagedReferences2, true};
    lua_State* L = luaL_newstate();
    
    lua_pushnumber(L, 42.0);
    int ref = lua_refpool(L, -1);
    lua_pop(L, 1);
    
    CHECK_NE(ref, LUA_REFNIL);
    
    lua_getrefpool(L, ref);
    CHECK(lua_isnumber(L, -1));
    CHECK_EQ(lua_tonumber(L, -1), 42.0);
    lua_pop(L, 1);
    
    lua_unrefpool(L, ref);
    
    // Check that dereferencing LUA_REFNIL yields nil
    lua_getrefpool(L, LUA_REFNIL);
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);

    lua_close(L);
}

TEST_CASE("RefPoolGC")
{
    ScopedFastFlag sff{FFlag::LuauManagedReferences2, true};
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // Create a table that we will reference
    lua_newtable(L);
    lua_pushnumber(L, 123.0);
    lua_setfield(L, -2, "x");
    
    int ref = lua_refpool(L, -1);
    lua_pop(L, 1);
    
    // Trigger full garbage collection
    lua_gc(L, LUA_GCCOLLECT, 0);
    
    // The reference should have protected the table from GC
    lua_getrefpool(L, ref);
    CHECK(lua_istable(L, -1));
    lua_getfield(L, -1, "x");
    CHECK_EQ(lua_tonumber(L, -1), 123.0);
    lua_pop(L, 2);
    
    // Unref and trigger GC again
    lua_unrefpool(L, ref);
    lua_gc(L, LUA_GCCOLLECT, 0);
    
    lua_close(L);
}

static int multiple_refs_dtor_hits = 0;

TEST_CASE("RefPoolMultipleRefs")
{
    ScopedFastFlag sff{FFlag::LuauManagedReferences2, true};
    lua_State* L = luaL_newstate();
    
    std::vector<int> refs;
    multiple_refs_dtor_hits = 0;
    
    // Allocate 100 refs
    for (int i = 0; i < 100; ++i)
    {
        void* ud = lua_newuserdatadtor(L, sizeof(int), [](lua_State*, void* data) {
            multiple_refs_dtor_hits++;
        });
        *(int*)ud = i;
        refs.push_back(lua_refpool(L, -1));
        lua_pop(L, 1);
    }
    
    // Verify they all have the correct values
    for (int i = 0; i < 100; ++i)
    {
        lua_getrefpool(L, refs[i]);
        CHECK(lua_isuserdata(L, -1));
        CHECK_EQ(*(int*)lua_touserdata(L, -1), i);
        lua_pop(L, 1);
    }
    
    // Trigger GC, they should not be collected
    lua_gc(L, LUA_GCCOLLECT, 0);
    CHECK_EQ(multiple_refs_dtor_hits, 0);
    
    // Unref first 50
    for (int i = 0; i < 50; ++i)
    {
        lua_unrefpool(L, refs[i]);
    }
    
    // Trigger GC, 50 should be collected
    lua_gc(L, LUA_GCCOLLECT, 0);
    CHECK_EQ(multiple_refs_dtor_hits, 50);
    
    // Allocate 50 more refs, ensuring the free-list works
    std::vector<int> refs2;
    for (int i = 0; i < 50; ++i)
    {
        void* ud = lua_newuserdatadtor(L, sizeof(int), [](lua_State*, void* data) {
            multiple_refs_dtor_hits++;
        });
        *(int*)ud = i + 100;
        refs2.push_back(lua_refpool(L, -1));
        lua_pop(L, 1);
    }
    
    for (int i = 0; i < 50; ++i)
    {
        lua_getrefpool(L, refs2[i]);
        CHECK_EQ(*(int*)lua_touserdata(L, -1), i + 100);
        lua_pop(L, 1);
    }
    
    for (int i = 0; i < 50; ++i)
    {
        lua_unrefpool(L, refs2[i]);
    }
    for (int i = 50; i < 100; ++i)
    {
        lua_unrefpool(L, refs[i]);
    }

    lua_gc(L, LUA_GCCOLLECT, 0);
    CHECK_EQ(multiple_refs_dtor_hits, 150);

    lua_close(L);
}

static int close_state_dtor_hits = 0;

TEST_CASE("RefPoolCloseState")
{
    ScopedFastFlag sff{FFlag::LuauManagedReferences2, true};
    lua_State* L = luaL_newstate();
    close_state_dtor_hits = 0;

    lua_newuserdatadtor(L, sizeof(int), [](lua_State*, void* data) {
        close_state_dtor_hits++;
    });
    
    // Pin it with a ref
    lua_refpool(L, -1);
    lua_pop(L, 1);

    // Garbage collection shouldn't collect it since it's ref'd
    lua_gc(L, LUA_GCCOLLECT, 0);
    CHECK_EQ(close_state_dtor_hits, 0);

    // Closing the state must clean up all remaining objects, even those held by refs
    lua_close(L);
    
    CHECK_EQ(close_state_dtor_hits, 1);
}

static int table_ref_gc_dtor_hits = 0;

TEST_CASE("RefPoolTableRefGC")
{
    ScopedFastFlag sff{FFlag::LuauManagedReferences2, true};
    lua_State* L = luaL_newstate();
    table_ref_gc_dtor_hits = 0;
    
    lua_newuserdatadtor(L, sizeof(int), [](lua_State*, void*) {
        table_ref_gc_dtor_hits++;
    });
    
    // Make a ref
    int ref = lua_refpool(L, -1);
    
    //  Make a table containing the val of the ref
    lua_newtable(L);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "val");
    
    // Drop the initial userdata from stack, leaving only the table on stack
    lua_remove(L, -2);
    
    // Do GC (which should not drop the ud bc table + ref)
    lua_gc(L, LUA_GCCOLLECT, 0);
    CHECK_EQ(table_ref_gc_dtor_hits, 0);
    
    // Drop ref
    lua_unrefpool(L, ref);
    
    // Do GC
    lua_gc(L, LUA_GCCOLLECT, 0);
    // Still 0 because the table holds it!
    CHECK_EQ(table_ref_gc_dtor_hits, 0); 
    
    // Drop table
    lua_pop(L, 1);
    
    // Do GC
    lua_gc(L, LUA_GCCOLLECT, 0);
    // Now it should be collected!
    CHECK_EQ(table_ref_gc_dtor_hits, 1); 
    
    lua_close(L);
}

TEST_CASE("RefPoolPrimitiveAPIs")
{
    ScopedFastFlag sff{FFlag::LuauManagedReferences2, true};
    lua_State* L = luaL_newstate();
    
    // Create a string
    lua_pushstring(L, "hello");
    int str_ref = lua_refpool(L, -1);
    lua_pop(L, 1);
    
    // Create a table
    lua_newtable(L);
    int table_ref1 = lua_refpool(L, -1);
    lua_pushvalue(L, -1);
    int table_ref2 = lua_refpool(L, -1);
    lua_pop(L, 2);
    
    // Create userdata
    void* ud = lua_newuserdata(L, 16);
    int ud_ref = lua_refpool(L, -1);
    lua_pop(L, 1);
    
    // Create nil
    lua_pushnil(L);
    int nil_ref = lua_refpool(L, -1);
    lua_pop(L, 1);
    
    // Test objlen
    CHECK_EQ(lua_refpool_objlen(L, str_ref), 5);
    CHECK_EQ(lua_refpool_objlen(L, table_ref1), 0);
    CHECK_EQ(lua_refpool_objlen(L, ud_ref), 16);
    CHECK_EQ(lua_refpool_objlen(L, nil_ref), 0);
    
    // Test topointer
    CHECK_NE(lua_refpool_topointer(L, table_ref1), nullptr);
    CHECK_EQ(lua_refpool_topointer(L, table_ref1), lua_refpool_topointer(L, table_ref2));
    CHECK_EQ(lua_refpool_topointer(L, ud_ref), ud);
    CHECK_EQ(lua_refpool_topointer(L, nil_ref), nullptr);
    CHECK_EQ(lua_refpool_topointer(L, LUA_REFNIL), nullptr);
    
    // Test rawequal
    CHECK_EQ(lua_refpool_rawequal(L, table_ref1, table_ref2), 1);
    CHECK_EQ(lua_refpool_rawequal(L, str_ref, table_ref1), 0);
    CHECK_EQ(lua_refpool_rawequal(L, nil_ref, nil_ref), 1);
    CHECK_EQ(lua_refpool_rawequal(L, LUA_REFNIL, LUA_REFNIL), 1);
    CHECK_EQ(lua_refpool_rawequal(L, table_ref1, LUA_REFNIL), 0);
    
    lua_close(L);
}

TEST_SUITE_END();
