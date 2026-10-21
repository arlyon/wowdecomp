#include "Engine/Source/lua-5.1/src/lua.h"
#include "Engine/Source/lua-5.1/src/lauxlib.h"
#include "Engine/Source/lua-5.1/src/lualib.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "Expected fixture and production Lua paths\n"; return 1; }
    auto* state = luaL_newstate();
    if (!state) return 1;
    luaL_openlibs(state);
    // The game intentionally omits Lua's filesystem loader. Expose only this
    // fixture's fixed production source, without changing the client sandbox.
    lua_pushstring(state, argv[2]);
    lua_pushcclosure(state, [](lua_State* lua) {
        if (luaL_loadfile(lua, lua_tostring(lua, lua_upvalueindex(1)))) return lua_error(lua);
        return 1;
    }, 1);
    lua_setglobal(state, "loadProduction");
    lua_createtable(state, 1, 0);
    lua_pushstring(state, argv[2]); lua_rawseti(state, -2, 1);
    lua_setglobal(state, "arg");
    const bool failed = luaL_loadfile(state, argv[1]) || lua_pcall(state, 0, 0, 0);
    if (failed) std::cerr << lua_tostring(state, -1) << '\n';
    else std::cout << "PASS native settings transaction, identity/capability failures, display retirement and idempotent installation\n";
    lua_close(state);
    return failed ? 1 : 0;
}
