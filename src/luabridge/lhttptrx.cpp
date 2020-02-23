#include <cstring>
#include "liocore.h"

using namespace std;

static int Lws_send(lua_State *L) {
    REFARG(1, websocket, self)
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    try {
        self->send(chunk(str, len));
    } RETHROW_LUA
    return 0;
}

static int Lws_read(lua_State *L) {
    REFARG(1, websocket, self)
    try {
        chunk c = self->read();
        if(c) {
            lua_pushlstring(L, c.data(), c.size());
        } else {
            lua_pushnil(L);
        }
        return 1;
    } RETHROW_LUA
}

static int Lws_poll(lua_State *L) {
    REFARG(1, websocket, self)
    try {
        lua_pushboolean(L, self->poll());
        return 1;
    } RETHROW_LUA
}

static void luaXYpush_websocket(lua_State *L, P<websocket> &tx) {
    auto *trx = (P<websocket> *)lua_newuserdata(L, sizeof(P<websocket>));
    new(trx) P<websocket>(tx);
    luaL_setmetatable(L, CXXMT(websocket));
}

static int Ltrx_write(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    try {
        self->write(str, len);
    } RETHROW_LUA
    return 0;
}

static int Ltrx_finish(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    // Allow finish to carry some data.
    if(!lua_isnoneornil(L, 2)) {
        lua_pushcfunction(L, Ltrx_write);
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 2);
        lua_call(L, 2, 0);
    }
    try {
        self->finish();
    } RETHROW_LUA
    return 0;
}

static int Ltrx_displayError(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    int code = luaL_checkinteger(L, 2);
    try {
        self->display_error(code);
    } RETHROW_LUA
    return 0;
}

static int Ltrx_serveFile(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    size_t len;
    const char *path = luaL_checklstring(L, 2, &len);
    try {
        self->serve_file(string(path, len));
    } RETHROW_LUA
    return 0;
}

static int Ltrx_forwardTo(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    size_t len;
    const char *addr = luaL_checklstring(L, 2, &len);
    int port = luaL_optinteger(L, 3, 80);
    try {
        self->forward_to(string(addr, len), port);
    } RETHROW_LUA
    return 0;
}

static int Ltrx_handleWith(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    const char *addr = luaL_checkstring(L, 2);
    try {
        P<fcgi_connection> conn;
        const char *portBase = strchr(addr, ':');
        if (!portBase || addr[0] == '/') {
            auto strm = make_shared<unix_stream>();
            strm->connect(addr);
            conn = make_shared<fcgi_connection>(strm, 1);
        } else {
            auto strm = make_shared<tcp_stream>();
            strm->connect(string(addr, portBase - addr), atoi(portBase));
            conn = make_shared<fcgi_connection>(strm, 1);
        }
        if (lua_istable(L, 3)) {
            lua_pushnil(L);  /* first key */
            while (lua_next(L, 3) != 0) {
                if (lua_isstring(L, -2)) {
                    size_t keylen, vallen;
                    const char *key = lua_tolstring(L, -2, &keylen);
                    const char *val = lua_tolstring(L, -1, &vallen);
                    conn->set_env(string(key, keylen), string(val, vallen));
                }
                lua_pop(L, 1);
            }
        }
        self->forward_to(move(conn));
    }
    catch(runtime_error &ex) {
        self->display_error(502);
    }
    RETHROW_LUA
    return 0;
}

static int Ltrx_redirectTo(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    size_t len;
    const char *path = luaL_checklstring(L, 2, &len);
    try {
        self->redirect_to(string(path, len));
    } RETHROW_LUA
    return 0;
}

static int Ltrx_acceptWebSocket(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    try {
        auto ws = self->accept_websocket();
        luaXYpush_websocket(L, ws);
        return 1;
    } RETHROW_LUA
}

static int Ltrx_header(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    size_t keyLen, valLen;
    if(lua_isnumber(L, 2)) {
        int statusCode = lua_tointeger(L, 2);
        try {
            auto resp = self->get_response(statusCode);
            if(lua_isstring(L, 3)) {
                const char *val = lua_tolstring(L, 3, &valLen);
                switch(statusCode) {
                case 301: case 302:
                    resp->set_header("Location", chunk(val, valLen));
                    break;
                case 401:
                    resp->set_header("WWW-Authenticate", chunk(val, valLen));
                    break;
                }
            }
        } RETHROW_LUA
        return 0;
    }
    const char *key = luaL_checklstring(L, 2, &keyLen);
    auto resp = self->get_response();
    try {
        if(lua_isnoneornil(L, 3)) {
            resp->delete_header(string(key, keyLen));
        } else {
            // TODO: Call unsafe Lua API before acquiring C++ resources
            const char *val = luaL_checklstring(L, 3, &valLen);
            resp->set_header(string(key, keyLen), string(val, valLen));
        }
    } RETHROW_LUA
    return 0;
}

static int Ltrx_peer(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    try {
        string peername = self->connection->peername();
        lua_pushlstring(L, peername.data(), peername.size());
        lua_pushboolean(L, self->connection->has_tls());
        return 2;
    } RETHROW_LUA
}

static int Ltrx_done(lua_State *L) {
    http_trx &self = luaXY_check<http_transaction>(L, 1, CXXMT(http_trx));
    try {
        lua_pushboolean(L, self->header_sent());
        return 1;
    } RETHROW_LUA
}

void luaXYpush_http_trx(lua_State *L, http_trx &tx) {
    auto *trx = (P<http_transaction> *)
            lua_newuserdata(L, sizeof(P<http_transaction>));
    new(trx) P<http_transaction>(tx);
    luaL_setmetatable(L, CXXMT(http_trx));
}

DEFINE_REFGC(http_transaction)
DEFINE_REFGC(websocket)

static luaL_Reg api_ws[] = {
        { "send", Lws_send },
        { "poll", Lws_poll },
        { "read", Lws_read },
        { NULL, NULL }
};

static luaL_Reg api[] = {
        { "acceptWebSocket", Ltrx_acceptWebSocket },
        { "displayError", Ltrx_displayError },
        { "finish", Ltrx_finish },
        { "forwardTo", Ltrx_forwardTo },
        { "handleWith", Ltrx_handleWith },
        { "header", Ltrx_header },
        { "peer", Ltrx_peer },
        { "redirectTo", Ltrx_redirectTo },
        { "serveFile", Ltrx_serveFile },
        { "write", Ltrx_write },
        { NULL, NULL }
};

int luaopen_http_trx(lua_State *L) {
    luaL_newmetatable(L, CXXMT(http_trx));
    lua_pushcfunction(L, Lgc_http_transaction);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, Ltrx_done);
    lua_setfield(L, -2, "__len");
    // Register APIs
    lua_newtable(L);
    luaL_setfuncs(L, api, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newmetatable(L, CXXMT(websocket));
    lua_pushcfunction(L, Lgc_websocket);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, api_ws, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    return 0;
}