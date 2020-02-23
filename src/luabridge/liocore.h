#ifndef XYHTTPD_LIOCORE_H
#define XYHTTPD_LIOCORE_H

#include <lua.hpp>
#include <xyhttp.h>

#define CXXMT(className) "C++" # className
#define REFARG(i, t, n) \
    auto &n = luaXY_check<t>(L, i, CXXMT(t));
#define RETHROW_LUA \
    catch(exception &ex) { return luaL_error(L, "%s", ex.what()); }
#define DEFINE_REFGC(tname) \
    static int Lgc_##tname(lua_State *L) { \
    auto ref = (shared_ptr<tname> *)lua_touserdata(L, 1); \
    ref->~shared_ptr<tname>(); \
    return 0; }

template<typename T>
inline P<T> &luaXY_check(lua_State *L, int idx, const char *metatype) {
    auto *ptr = (P<T> *)luaL_checkudata(L, idx, metatype);
    return *ptr;
}

int luaopen_http_trx(lua_State *L);
void luaXYpush_http_trx(lua_State *L, http_trx &tx);

#endif //XYHTTPD_LIOCORE_H
