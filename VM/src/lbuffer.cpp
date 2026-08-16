// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lbuffer.h"

#include "lgc.h"
#include "lmem.h"

#include "lstate.h"
#include "ldo.h"
#include <string.h>

Buffer* luaB_newbuffer(lua_State* L, size_t s)
{
    if (s > MAX_BUFFER_SIZE)
        luaM_toobig(L, "buffer too big", MAX_BUFFER_SIZE);

    Buffer* b = luaM_newgco(L, Buffer, sizebuffer(s), L->activememcat);
    luaC_init(L, b, LUA_TBUFFER);
    b->len = unsigned(s);
    b->mode = 0;
    b->tag = 0;
    b->data = b->inline_data;
    b->userdata = NULL;
    memset(b->data, 0, b->len);
    return b;
}

Buffer* luaB_newexternalbuffer(lua_State* L, size_t s, void* data, void* userdata, int tag, int mode)
{
    LUAU_ASSERT(mode == 1 || mode == 2);
    lua_BufferFree dtor = L->global->buffergc[tag];
    if (s > MAX_BUFFER_SIZE)
    {
        if (dtor)
            dtor(L, data, s, userdata);
        luaM_toobig(L, "ext. buffer too big", MAX_BUFFER_SIZE);
    }

    struct AllocContext
    {
        Buffer* b = nullptr;

        static void run(lua_State* L, void* ud)
        {
            AllocContext* ctx = static_cast<AllocContext*>(ud);
            ctx->b = luaM_newgco(L, Buffer, offsetof(Buffer, inline_data), L->activememcat);
        }
    } ctx;

    int status = luaD_rawrunprotected(L, &AllocContext::run, &ctx);
    if (status != 0)
    {
        if (dtor)
            dtor(L, data, s, userdata);
        luaD_throw(L, status);
    }

    Buffer* b = ctx.b;
    luaC_init(L, b, LUA_TBUFFER);
    b->len = unsigned(s);
    b->mode = mode;
    b->data = (char*)data;
    b->userdata = userdata;
    b->tag = tag;
    
    L->global->totalbytes += s;
    L->global->memcatbytes[L->activememcat] += s;
    return b;
}

void luaB_freebuffer(lua_State* L, Buffer* b, lua_Page* page)
{
    if (b->mode != 0) {
        lua_BufferFree dtor = L->global->buffergc[b->tag];
        if (dtor) {
            dtor(L, b->data, b->len, b->userdata);
        }
        L->global->totalbytes -= b->len;
        L->global->memcatbytes[b->memcat] -= b->len;
        luaM_freegco(L, b, offsetof(Buffer, inline_data), b->memcat, page);
    } else {
        luaM_freegco(L, b, sizebuffer(b->len), b->memcat, page);
    }
}
