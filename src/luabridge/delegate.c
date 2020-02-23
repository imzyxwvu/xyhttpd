#include <stdlib.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

int Lmain(lua_State *L) {
    const char *prog = luaL_checkstring(L, 1);
    int n = lua_gettop(L);
    int status;
    luaL_openlibs(L);
    status = luaL_loadfile(L, prog);
    if(status != LUA_OK) {
        fprintf(stderr, "loadfile: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return luaL_error(L, "error compiling \"%s\"", prog);
    }
    for(int i = 1; i < n; i++)
        lua_pushvalue(L, i);
    lua_call(L, n - 1, 0);
    return 0;
}

int main(int argc, char *argv[])
{
    lua_State *L;
    int status;

    if(argc < 2) {
        fprintf(stderr,"Usage: %s <main.lua>\n", argv[0]);
        return 0;
    }
    L = luaL_newstate();
    if(L == NULL) {
        fprintf(stderr, "delegate: error while initializing Lua\n");
        return EXIT_FAILURE;
    }
    lua_pushcfunction(L, Lmain);
    for(int i = 1; i < argc; i++)
        lua_pushstring(L, argv[i]);
    status = lua_pcall(L, argc - 1, 0, 0);
    if(status != LUA_OK) {
        fprintf(stderr, "%s: %s\n", argv[0], lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_close(L);
    return status == LUA_OK ? 0 : EXIT_FAILURE;
}