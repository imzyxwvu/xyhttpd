#include <iostream>
#include <cstring>
#include <xyhttptls.h>
#include <algorithm>
#include "liocore.h"

using namespace std;

static uv_signal_t sig; // SIGTERM handler
static uv_prepare_t gchook; // Make GC called every iteration
static queue<int> suspend_ref;

class lua_http_service : public http_service {
public:
    lua_http_service(lua_State *, int);
    inline int callbackRef() { return _callbackRef; }
    virtual void serve(http_trx &tx);
private:
    lua_State *_mainState;
    int _callbackRef;
};

lua_http_service::lua_http_service(lua_State *L, int cbidx) : _mainState(L) {
    lua_pushvalue(L, cbidx);
    _callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
}

void lua_http_service::serve(http_trx &tx) {
    lua_State *L = lua_newthread(_mainState);
    lua_rawgeti(L, LUA_REGISTRYINDEX, _callbackRef);
    // Push request brief
    auto *req = (P<http_request> *)lua_newuserdata(L, sizeof(P<http_request>));
    new(req) P<http_request>(tx->request);
    luaL_setmetatable(L, CXXMT(http_request));
    // Push transaction itself
    luaXYpush_http_trx(L, tx);
    int ref = luaL_ref(_mainState, LUA_REGISTRYINDEX); // Prevent GC
    int status = lua_pcall(L, 2, 0, 0);
    suspend_ref.push(ref);
    if(status != LUA_OK) {
        throw runtime_error(fmt("Lua error: %s", lua_tostring(L, -1)));
    }
}

static int Llisten(lua_State *L) {
    const char *ip = luaL_checkstring(L, 1);
    int port = luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    auto *server = (http_server *)lua_newuserdata(L, sizeof(http_server));
    try {
        new(server) http_server(make_shared<lua_http_service>(L, 3));
        luaL_setmetatable(L, CXXMT(http_server));
        server->listen(ip, port);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushinteger(L, ref);
        return ref;
    }
    catch(exception &ex) {
        return luaL_error(L, "listen: %s", ex.what());
    }
}

static int Ltls_listen(lua_State *L) {
    const char *ip = luaL_checkstring(L, 1);
    int port = luaL_checkinteger(L, 2);
    auto &ctx = luaXY_check<tls_context>(L, 3, CXXMT(tls_context));
    luaL_checktype(L, 4, LUA_TFUNCTION);
    auto *server = (https_server *)lua_newuserdata(L, sizeof(https_server));
    try {
        new(server) https_server(ctx, make_shared<lua_http_service>(L, 4));
        luaL_setmetatable(L, CXXMT(http_server));
        server->listen(ip, port);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushinteger(L, ref);
        return ref;
    }
    catch(exception &ex) {
        return luaL_error(L, "listen: %s", ex.what());
    }
}

int Ltctx_register(lua_State *L) {
    REFARG(1, tls_context, self)
    const char *hostname = luaL_checkstring(L, 2);
    if(lua_isnoneornil(L, 3)) {
        self->unregister_context(hostname);
    } else {
        auto &other = luaXY_check<tls_context>(L, 3, CXXMT(tls_context));
        self->register_context(hostname, other);
    }
    return 0;
}

int Lreq__index(lua_State *L) {
    REFARG(1, http_request, self)
    if(lua_isnumber(L, 2) && lua_tointeger(L, 2) == 1) {
        lua_pushlstring(L, self->resource().data(), self->resource().size());
        return 1;
    }
    size_t keyLen;
    const char *key = luaL_checklstring(L, 2, &keyLen);
    if(keyLen == 4 && strcmp(key, "path") == 0)
        lua_pushlstring(L, self->path().data(), self->path().size());
    else if(self->query() && strcmp(key, "query") == 0)
        lua_pushlstring(L, self->query().data(), self->query().size());
    else {
        auto val = self->header(string(key, keyLen));
        if(val) {
            lua_pushlstring(L, val.data(), val.size());
        } else {
            lua_pushnil(L);
        }
    }
    return 1;
}

int Lreq__newindex(lua_State *L) {
    REFARG(1, http_request, self)
    size_t keyLen, valLen;
    const char *key = luaL_checklstring(L, 2, &keyLen);
    if(lua_isnoneornil(L, 3)) {
        self->delete_header(string(key, keyLen));
    } else {
        const char *val = luaL_checklstring(L, 3, &valLen);
        if(valLen == 4 && strcmp(key, "path")) {
            self->set_resource(chunk(val, valLen));
        } else {
            self->set_header(string(key, keyLen), chunk(val, valLen));
        }
    }
    return 0;
}

int Lreq__len(lua_State *L) {
    REFARG(1, http_request, self)
    lua_pushlstring(L, self->method.data(),  self->method.size());
    return 1;
}

int Ltls_context(lua_State *L) {
    const char *x509file = luaL_checkstring(L, 1);
    const char *keyfile = luaL_checkstring(L, 2);
    P<tls_context> ctx;
    try {
        ctx = make_shared<tls_context>(x509file, keyfile);
    } RETHROW_LUA
    auto *trx = (P<tls_context> *)
            lua_newuserdata(L, sizeof(P<tls_context>));
    new(trx) P<tls_context>(move(ctx));
    luaL_setmetatable(L, CXXMT(tls_context));
    return 1;
}

static void stop_loop(uv_signal_t* handle, int signum) {
    uv_stop(uv_default_loop());
}

static void invoke_gc(uv_prepare_t* handle) {
    auto *L = (lua_State *)handle->data;
    while(!suspend_ref.empty()) {
        int gref = suspend_ref.front();
        suspend_ref.pop();
        luaL_unref(L, LUA_REGISTRYINDEX, gref);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
}

static int Lrun(lua_State *L) {
    uv_signal_start_oneshot(&sig, stop_loop, SIGTERM);
    gchook.data = L;
    uv_prepare_start(&gchook, invoke_gc);
    try {
        uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    } RETHROW_LUA
    uv_prepare_stop(&gchook);
    return 0;
}

static int Lwrap(lua_State *L) {
    int top = lua_gettop(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State *newthread = lua_newthread(L);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    for(int i = 1; i <= top; i++)
        lua_pushvalue(L, i);
    lua_xmove(L, newthread, top);
    fiber::launch([L, newthread, top, ref] () {
        int status = lua_pcall(newthread, top - 1, 0, 0);
        suspend_ref.push(ref);
        if(status != LUA_OK) {
            throw runtime_error(fmt("Lua error: %s", lua_tostring(L, -1)));
        }
    });
    return 0;
}

#ifdef _WIN32
static const char *mode2string (unsigned short mode) {
#else
static const char *mode2string (mode_t mode) {
#endif
    if ( S_ISREG(mode) )
        return "file";
    else if ( S_ISDIR(mode) )
        return "directory";
    else if ( S_ISLNK(mode) )
        return "link";
    else if ( S_ISSOCK(mode) )
        return "socket";
    else if ( S_ISFIFO(mode) )
        return "named pipe";
    else if ( S_ISCHR(mode) )
        return "char device";
    else if ( S_ISBLK(mode) )
        return "block device";
    else
        return "other";
}

static int Lstat(lua_State *L) {
    struct stat info;
    const char *file = luaL_checkstring (L, 1);

    if (stat(file, &info)) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushstring(L, mode2string (info.st_mode));
    lua_pushinteger(L, (lua_Integer) info.st_mtime);
    return 2;
}

static int Lurl_decode(lua_State *L) {
    size_t len;
    const char *str = luaL_checklstring(L, 1, &len);
    string result = http_request::url_decode(str, len);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int Lgc_http_server(lua_State *L) {
    auto *server = (http_server *)lua_touserdata(L, 1);
    server->~http_server();
    return 0;
}

static int Lclnt_data_available(lua_State *L) {
    REFARG(1, http_client, self);
    lua_pushboolean(L, self->data_available());
    return 1;
}

static void update_request_from_table(lua_State *L, int idx, P<http_request> request) {
    size_t len;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if(lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue; // Silently skip invalid headers
        }
        const char *headerKeyPtr = lua_tolstring(L, -2, &len);
        string headerKey(headerKeyPtr, len);
        const char *headerValue = lua_tolstring(L, -1, &len);
        if(headerKey == "method") { // Specify request method
            request->method = string(headerValue, len);
            transform(request->method.begin(), request->method.end(),
                      request->method.begin(),::toupper);
        } else {
            request->set_header(headerKey, chunk(headerValue, len));
        }
        lua_pop(L, 1);
    }
}

static void Lpush_response(lua_State *L, const P<http_response> &response) {
    lua_createtable(L, 1, 8);
    lua_pushinteger(L, response->code());
    lua_rawseti(L, -2, 1);
    for(auto it = response->hbegin(); it != response->hend(); it++) {
        lua_pushlstring(L, it->first.data(), it->first.size());
        lua_pushlstring(L, it->second.data(), it->second.size());
        lua_rawset(L, -3);
    }
}

static int Lclnt_send(lua_State *L) {
    REFARG(1, http_client, self);
    if(lua_isuserdata(L, 2)) {
        REFARG(2, http_request, request);
        try {
            auto response = self->send(request);
            Lpush_response(L, response);
            return 1;
        } RETHROW_LUA
    }
    // Verify resource path type and load
    luaL_checktype(L, 2, LUA_TTABLE);
    size_t len;
    lua_rawgeti(L, 2, 1);
    if(!lua_isstring(L, -1))
        return luaL_error(L, "resource path expects string");
    const char *resource = lua_tolstring(L, -1, &len);
    auto request = make_shared<http_request>();
    request->set_resource(chunk(resource, len));
    lua_pop(L, 1);
    update_request_from_table(L, 2, request);
    if(request->method.empty())
        request->method = "GET";
    try {
        auto response = self->send(request);
        Lpush_response(L, response);
        return 1;
    } RETHROW_LUA
}

static int Lhttp_open(lua_State *L) {
    size_t len;
    const char *addr = luaL_checklstring(L, 1, &len);
    int port = luaL_optinteger(L, 2, 80); // Default HTTP port 80
    try {
        auto strm = make_shared<tcp_stream>();
        strm->connect(string(addr, len), port);

        auto *client = (P<http_client> *)
                lua_newuserdata(L, sizeof(P<http_client>));
        new(client) P<http_client>(make_shared<http_client>(strm));
        luaL_setmetatable(L, CXXMT(http_client));
        return 1;
    } RETHROW_LUA
}

static int Lclnt_read(lua_State *L) {
    REFARG(1, http_client, self);
    try {
        auto data = self->read();
        if(data) {
            lua_pushlstring(L, data.data(), data.size());
        } else {
            lua_pushnil(L);
        }
        return 1;
    } RETHROW_LUA
}

static int Lclnt_readall(lua_State *L) {
    REFARG(1, http_client, self);
    try {
        stream_buffer sb;
        while(self->data_available()) {
            chunk c = self->read();
            sb.append(c.data(), c.size());
        }
        lua_pushlstring(L, sb.data(), sb.size());
        return 1;
    } RETHROW_LUA
}

static luaL_Reg Lhttpclnt_api[] = {
        { "send", Lclnt_send },
        { "read", Lclnt_read },
        { "readall", Lclnt_readall },
        { NULL, NULL }
};

DEFINE_REFGC(tls_context)
DEFINE_REFGC(http_request)
DEFINE_REFGC(http_client)

static void setup_metatables(lua_State *L) {
    luaopen_http_trx(L);

    luaL_newmetatable(L, CXXMT(http_server));
    lua_pushcfunction(L, Lgc_http_server);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    luaL_newmetatable(L, CXXMT(tls_context));
    lua_pushcfunction(L, Lgc_tls_context);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, Ltctx_register);
    lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);

    luaL_newmetatable(L, CXXMT(http_request));
    lua_pushcfunction(L, Lgc_http_request);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, Lreq__index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Lreq__newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Lreq__len);
    lua_setfield(L, -2, "__len");
    lua_pop(L, 1);

    luaL_newmetatable(L, CXXMT(http_client));
    lua_pushcfunction(L, Lgc_http_client);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, Lclnt_data_available);
    lua_setfield(L, -2, "__len");
    // Register APIs
    lua_newtable(L);
    luaL_setfuncs(L, Lhttpclnt_api, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

extern "C" {

static luaL_Reg lib[] = {
        { "listen", Llisten },
        { "tls_listen", Ltls_listen },
        { "tls_context", Ltls_context },
        { "http_open", Lhttp_open },
        { "run", Lrun },
        { "wrap", Lwrap },
        { "stat", Lstat },
        { "url_decode", Lurl_decode },
        { NULL, NULL }
};

LUALIB_API int luaopen_liocore(lua_State *L) {
    SSL_library_init();
    if(uv_prepare_init(uv_default_loop(), &gchook) < 0)
        luaL_error(L, "uv_prepare_init < 0");
    // Setup Signal Handlers
    signal(SIGPIPE, SIG_IGN);
    if(uv_signal_init(uv_default_loop(), &sig) < 0)
        luaL_error(L, "uv_signal_init < 0");

    setup_metatables(L);
    lua_newtable(L);
    luaL_setfuncs(L, lib, 0);
    return 1;
}

}