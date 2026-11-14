#include "cedit/script.h"

#include "cedit/editor.h"

#ifndef CEDIT_LUA

bool script_available(void) { return false; }
void script_init(Editor *e) { (void)e; }
void script_free(Editor *e) { (void)e; }
void script_hook(Editor *e, const char *event, Buffer *b) {
    (void)e;
    (void)event;
    (void)b;
}
void script_run_command(Editor *e, int ref) {
    (void)ref;
    editor_error(e, "Built without Lua support");
}
void script_eval(Editor *e, const char *code) {
    (void)code;
    editor_error(e, "Built without Lua support");
}

#else

#include <dirent.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define L_OF(e) ((lua_State *)(e)->script)

static Editor *self(lua_State *L) {
    return lua_touserdata(L, lua_upvalueindex(1));
}

static time_t call_started;

/* Abort scripts that run for more than 5 seconds. */
static void watchdog(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (time(NULL) - call_started > 5) luaL_error(L, "script timed out");
}

static void report(Editor *e, lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    char line[256];
    snprintf(line, sizeof line, "%s", msg ? msg : "Lua error");
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    editor_error(e, "Lua: %s", line);
    lua_pop(L, 1);
}

static int traceback(lua_State *L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

/* Calls the function below nargs arguments on the stack. */
static bool pcall(Editor *e, lua_State *L, int nargs) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    call_started = time(NULL);
    int rc = lua_pcall(L, nargs, 0, base);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        report(e, L);
        return false;
    }
    return true;
}

static int l_message(lua_State *L) {
    editor_message(self(L), "%s", luaL_checkstring(L, 1));
    return 0;
}

static int l_insert(lua_State *L) {
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    lua_pushboolean(L, insert_text(self(L), s, n));
    return 1;
}

static int l_line_count(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)text_lines(&editor_buf(self(L))->text));
    return 1;
}

static int l_get_line(lua_State *L) {
    Buffer *b = editor_buf(self(L));
    lua_Integer n = luaL_optinteger(L, 1, (lua_Integer)buffer_cur_line(b) + 1);
    if (n < 1 || (size_t)n > text_lines(&b->text)) return 0;
    size_t s = text_line_start(&b->text, (size_t)n - 1);
    size_t t = text_line_end(&b->text, (size_t)n - 1);
    lua_pushlstring(L, text_view(&b->text, s, t - s), t - s);
    return 1;
}

static int l_cursor(lua_State *L) {
    Buffer *b = editor_buf(self(L));
    size_t line = buffer_cur_line(b);
    lua_pushinteger(L, (lua_Integer)line + 1);
    lua_pushinteger(L,
                    (lua_Integer)(b->cursor - text_line_start(&b->text, line)) + 1);
    return 2;
}

static int l_set_cursor(lua_State *L) {
    Buffer *b = editor_buf(self(L));
    lua_Integer line = luaL_checkinteger(L, 1), col = luaL_optinteger(L, 2, 1);
    size_t n = text_lines(&b->text);
    if (line < 1) line = 1;
    if ((size_t)line > n) line = (lua_Integer)n;
    size_t s = text_line_start(&b->text, (size_t)line - 1);
    size_t t = text_line_end(&b->text, (size_t)line - 1);
    size_t c = col < 1 ? 0 : (size_t)col - 1;
    b->cursor = s + (c > t - s ? t - s : c);
    /* never leave the cursor inside a UTF-8 sequence */
    while (b->cursor > s && (text_byte(&b->text, b->cursor) & 0xC0) == 0x80)
        b->cursor--;
    return 0;
}

static int l_filename(lua_State *L) {
    Buffer *b = editor_buf(self(L));
    if (b->path)
        lua_pushstring(L, b->path);
    else
        lua_pushnil(L);
    return 1;
}

static int l_buffer_name(lua_State *L) {
    lua_pushstring(L, editor_buf(self(L))->name);
    return 1;
}

static int l_selection(lua_State *L) {
    Buffer *b = editor_buf(self(L));
    size_t s, t;
    if (!buffer_region(b, &s, &t)) return 0;
    char *d = text_dup(&b->text, s, t - s);
    if (!d) return luaL_error(L, "out of memory");
    lua_pushlstring(L, d, t - s);
    free(d);
    return 1;
}

static int l_run(lua_State *L) {
    lua_pushboolean(L, command_run(self(L), luaL_checkstring(L, 1)));
    return 1;
}

static int l_set(lua_State *L) {
    char err[256];
    if (config_apply_line(self(L), luaL_checkstring(L, 1), err, sizeof err))
        return luaL_error(L, "%s", err);
    return 0;
}

static int l_bind(lua_State *L) {
    char err[256];
    if (keymap_bind(self(L)->keymap, luaL_checkstring(L, 1),
                    luaL_checkstring(L, 2), err, sizeof err))
        return luaL_error(L, "%s", err);
    return 0;
}

static int l_command(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    command_register(self(L), name, NULL, "(Lua command)", 0, ref);
    return 0;
}

static int l_on(lua_State *L) {
    const char *ev = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "cedit.hooks");
    if (lua_getfield(L, -1, ev) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, ev);
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    return 0;
}

static const luaL_Reg api[] = {
    {"message", l_message},       {"insert", l_insert},
    {"line_count", l_line_count}, {"get_line", l_get_line},
    {"cursor", l_cursor},         {"set_cursor", l_set_cursor},
    {"filename", l_filename},     {"buffer_name", l_buffer_name},
    {"selection", l_selection},   {"run", l_run},
    {"set", l_set},               {"bind", l_bind},
    {"command", l_command},       {"on", l_on},
    {NULL, NULL},
};

bool script_available(void) { return true; }

/* Refuse files others could have tampered with. */
static bool trusted(Editor *e, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 022)) {
        editor_error(e, "Lua: refusing %s (must be owned by you and not "
                        "group/world-writable)", path);
        return false;
    }
    return true;
}

static void load_file(Editor *e, const char *path) {
    lua_State *L = L_OF(e);
    if (!trusted(e, path)) return;
    if (luaL_loadfile(L, path) != LUA_OK) {
        report(e, L);
        return;
    }
    pcall(e, L, 0);
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void script_init(Editor *e) {
    lua_State *L = luaL_newstate();
    if (!L) {
        editor_error(e, "Lua: cannot create state");
        return;
    }
    luaL_openlibs(L);
    lua_sethook(L, watchdog, LUA_MASKCOUNT, 1000000);
    e->script = L;
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "cedit.hooks");
    lua_newtable(L);
    lua_pushlightuserdata(L, e);
    luaL_setfuncs(L, api, 1);
    lua_setglobal(L, "cedit");

    char *init = config_path("init.lua");
    if (init && access(init, F_OK) == 0) load_file(e, init);
    free(init);

    if (!e->cfg.load_plugins) return;
    char *dir = config_path("plugins");
    DIR *d = dir ? opendir(dir) : NULL;
    if (d) {
        char **names = NULL;
        size_t n = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t l = strlen(de->d_name);
            if (l > 4 && strcmp(de->d_name + l - 4, ".lua") == 0) {
                names = xreallocarray(names, n + 1, sizeof *names);
                names[n++] = xstrdup(de->d_name);
            }
        }
        closedir(d);
        qsort(names, n, sizeof *names, cmp_names);
        for (size_t i = 0; i < n; i++) {
            Str p = {0};
            str_appendf(&p, "%s/%s", dir, names[i]);
            load_file(e, p.data);
            str_free(&p);
            free(names[i]);
        }
        free(names);
    }
    free(dir);
}

void script_free(Editor *e) {
    if (e->script) lua_close(L_OF(e));
    e->script = NULL;
}

void script_hook(Editor *e, const char *event, Buffer *b) {
    lua_State *L = L_OF(e);
    if (!L) return;
    lua_getfield(L, LUA_REGISTRYINDEX, "cedit.hooks");
    if (lua_getfield(L, -1, event) != LUA_TTABLE) {
        lua_pop(L, 2);
        return;
    }
    lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_pushstring(L, b ? b->name : "");
        pcall(e, L, 1);
    }
    lua_pop(L, 2);
}

void script_run_command(Editor *e, int ref) {
    lua_State *L = L_OF(e);
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    pcall(e, L, 0);
}

void script_eval(Editor *e, const char *code) {
    lua_State *L = L_OF(e);
    if (!L) return;
    /* try as an expression first so "1+1" shows a result */
    Str chunk = {0};
    str_appendf(&chunk, "local r = (%s) if r ~= nil then cedit.message(tostring(r)) end", code);
    if (luaL_loadstring(L, chunk.data) != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadstring(L, code) != LUA_OK) {
            report(e, L);
            str_free(&chunk);
            return;
        }
    }
    str_free(&chunk);
    pcall(e, L, 0);
}

#endif
