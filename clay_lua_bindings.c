#define CLAY_IMPLEMENTATION
#include "../clay/clay.h"

#include "lua.h"
#include "lauxlib.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef LUA_TCDATA
#define LUA_TCDATA 10 /* LuaJIT specific */
#endif

/* --- Lua 5.1 / LuaJIT compatibility for lua_absindex --- */
#if LUA_VERSION_NUM < 502 && !defined(lua_absindex)
static inline int lua_absindex(lua_State *L, int idx) {
    return (idx > 0 || idx <= LUA_REGISTRYINDEX) ? idx : (lua_gettop(L) + idx + 1);
}
#endif

// Lua 5.1 / LuaJIT compatibility shims
#ifndef LUA_OK
#define LUA_OK 0
#endif

typedef struct ClayOwnedContext {
    void *memory;
    size_t capacity;
    Clay_Context *context;
    struct ClayOwnedContext *next;
} ClayOwnedContext;

static ClayOwnedContext *g_ClayOwnedContexts = NULL;
static int g_ClayAliasRegistrarRef = LUA_NOREF;
static Clay_Context *g_ClayBuildingContext = NULL;

typedef struct ClayKeyScope {
    Clay_Context *context;
    uint32_t parentId;
    uint32_t keyParentId;
    uint32_t nextOffset;
    struct ClayKeyScope *previous;
} ClayKeyScope;
static ClayKeyScope *g_ClayKeyScope = NULL;

static Clay_LayoutElement *clay_current_open_element(Clay_Context *context) {
    if (!context || context->openLayoutElementStack.length < 1) return NULL;
    return Clay__GetOpenLayoutElement();
}

static uint32_t clay_auto_root_parent_id(void) {
    return Clay__HashString(CLAY_STRING("clay_lua_auto_root"), 0).id;
}

static int clay_register_owned_context(void *memory, size_t capacity, Clay_Context *context) {
    ClayOwnedContext *owned = (ClayOwnedContext *)malloc(sizeof(*owned));
    if (!owned) return 0;
    owned->memory = memory;
    owned->capacity = capacity;
    owned->context = context;
    owned->next = g_ClayOwnedContexts;
    g_ClayOwnedContexts = owned;
    return 1;
}

static ClayOwnedContext *clay_find_owned_context(Clay_Context *context, ClayOwnedContext **previous) {
    ClayOwnedContext *prev = NULL;
    for (ClayOwnedContext *owned = g_ClayOwnedContexts; owned; owned = owned->next) {
        if (owned->context == context) {
            if (previous) *previous = prev;
            return owned;
        }
        prev = owned;
    }
    if (previous) *previous = NULL;
    return NULL;
}


// ---- Helpers
static Clay_String Clay_CopyLuaString(lua_State *L, int index) {
    size_t len = 0;
    const char *txt = luaL_checklstring(L, index, &len);
    Clay_Context* ctx = Clay_GetCurrentContext();
    if (!ctx) {
		luaL_error(L, "Clay context is null (did you call clay.initialize()?)");
	}
    Clay_String tmp = { .chars = txt, .length = (int32_t)len, .isStaticallyAllocated = false };
    return Clay__WriteStringToCharBuffer(&ctx->dynamicStringData, tmp);
}

// Borrow Lua string memory (no Clay allocations). Safe for immediate hashing calls.
static inline Clay_String Clay_BorrowLuaString(lua_State *L, int index) {
    size_t len = 0;
    const char *txt = luaL_checklstring(L, index, &len);
    Clay_String s = {0};
    s.chars = txt;
    s.length = (int32_t)len;
    s.isStaticallyAllocated = true; // important: don't assume Clay owns this
    return s;
}

static inline int clay_is_ref_tag(void *p) {
    return ((uintptr_t)p & (uintptr_t)1u) != 0;
}

static inline int clay_ref_from_tag(void *p) {
    return (int)((uintptr_t)p >> 1);             // strip tag
}

static inline void* clay_tag_from_ref(int ref) {
    return (void*)(((uintptr_t)ref << 1) | 1u);  // set tag
}

// Unref a tagged registry reference stored in a void* field.
// Safe to call with NULL.
static inline void clay_unref_tagged(lua_State *L, void **field) {
    if (!field) return;
    void *p = *field;
    if (p && clay_is_ref_tag(p)) {
        int ref = clay_ref_from_tag(p);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        *field = NULL;
    }
}

// Set a void* field from a Lua value:
// - nil => NULL
// - lightuserdata => raw pointer
// - anything else => store registry ref and tag it into the pointer
// If overwriting an existing tagged ref, it is unref'd (only safe before transfer to Clay).
static inline void clay_set_ptr_from_lua(lua_State *L, int value_index, void **field) {
    value_index = lua_absindex(L, value_index);

    // If we're overwriting a previous un-transferred tagged value, release it.
    clay_unref_tagged(L, field);

    if (lua_isnil(L, value_index)) {
        *field = NULL;
        return;
    }

    if (lua_islightuserdata(L, value_index)) {
        *field = lua_touserdata(L, value_index);
        return;
    }

    lua_pushvalue(L, value_index);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    *field = clay_tag_from_ref(ref);
}

/* -----------------------------------------------------------------------------
   Color parsing helper (nil-tolerant)

   Accepts:
     - nil => return 0 (no-op)
     - table array {r,g,b,a} or keyed {r=,g=,b=,a=} (a optional)
     - packed integer: 0xRRGGBBAA
     - numbers r,g,b[,a] (a defaults 255)

   The behavior for packed vs rgba numbers is based on arg count at call site.
   For fluent methods, idx is the first argument after "self".
----------------------------------------------------------------------------- */
static int clay_read_color(lua_State *L, int idx, Clay_Color *out) {
    idx = lua_absindex(L, idx);

    if (lua_isnil(L, idx))
        return 0;

    // Table form
    if (lua_istable(L, idx)) {
        // Array form: {r,g,b,a}
        lua_rawgeti(L, idx, 1); out->r = (float)luaL_optnumber(L, -1, 0); lua_pop(L, 1);
        lua_rawgeti(L, idx, 2); out->g = (float)luaL_optnumber(L, -1, 0); lua_pop(L, 1);
        lua_rawgeti(L, idx, 3); out->b = (float)luaL_optnumber(L, -1, 0); lua_pop(L, 1);
        lua_rawgeti(L, idx, 4); out->a = (float)luaL_optnumber(L, -1, 0); lua_pop(L, 1);

        // If array didn't exist, allow keyed form.
        // (We can't reliably detect "array absent", but keyed values will override)
        lua_getfield(L, idx, "r"); if (lua_isnumber(L,-1)) out->r = (float)lua_tonumber(L,-1); lua_pop(L,1);
        lua_getfield(L, idx, "g"); if (lua_isnumber(L,-1)) out->g = (float)lua_tonumber(L,-1); lua_pop(L,1);
        lua_getfield(L, idx, "b"); if (lua_isnumber(L,-1)) out->b = (float)lua_tonumber(L,-1); lua_pop(L,1);
        lua_getfield(L, idx, "a"); if (lua_isnumber(L,-1)) out->a = (float)lua_tonumber(L,-1); lua_pop(L,1);
        return 1;
    }

    // Numeric forms
    if (!lua_isnumber(L, idx)) {
        luaL_error(L, "expected color as nil|table|number(s)");
        return 0;
    }

    int remaining = lua_gettop(L) - idx + 1;

    // Packed hex if it's the only numeric argument at this idx (or if caller wants packed+width, handled at call site).
    if (remaining == 1) {
        uint32_t hex = (uint32_t)lua_tointeger(L, idx);

        // passing 0 must be intentional; set transparent
        if (hex == 0u) {
            out->r = 0.0f;
            out->g = 0.0f;
            out->b = 0.0f;
            out->a = 0.0f;
            return 1;
        }

        out->r = (float)((hex >> 24) & 0xFF);
        out->g = (float)((hex >> 16) & 0xFF);
        out->b = (float)((hex >> 8) & 0xFF);
        out->a = (float)((hex) & 0xFF);
        return 1;
    }

    // r,g,b[,a]
    if (remaining >= 3) {
        out->r = (float)luaL_checknumber(L, idx);
        out->g = (float)luaL_checknumber(L, idx + 1);
        out->b = (float)luaL_checknumber(L, idx + 2);
        out->a = (float)luaL_optnumber(L, idx + 3, 255.0f);
        return 1;
    }

    // remaining==2 is ambiguous (packed+something). Caller should handle packed+width, but as a fallback treat first as packed.
    {
        uint32_t hex = (uint32_t)lua_tointeger(L, idx);
        out->r = (float)((hex >> 24) & 0xFF);
        out->g = (float)((hex >> 16) & 0xFF);
        out->b = (float)((hex >> 8) & 0xFF);
        out->a = (float)((hex) & 0xFF);
        return 1;
    }
}

// ---- Measure bridge (safe, no baseChars arithmetic, no Clay calls inside) ----
static lua_State *g_LuaState = NULL;
static int g_MeasureTextRef = LUA_NOREF;

static Clay_Dimensions Bridge_MeasureTextFunction(Clay_StringSlice s, Clay_TextElementConfig* cfg, void* userdata) {
    (void)userdata;
    if (g_MeasureTextRef == LUA_NOREF) {
        return (Clay_Dimensions) {
                .width = s.length * (cfg ? cfg->fontSize : 16),
                .height = cfg ? cfg->fontSize : 16
        };
    }

    lua_State *L = g_LuaState;
    if (!lua_checkstack(L, 12)) {
        fprintf(stderr, "[clay] not enough Lua stack space for measureText callback\n");
        return (Clay_Dimensions){1.0f, 1.0f};
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_MeasureTextRef);
    lua_pushlstring(L, s.chars, (size_t)s.length);
    if (cfg) {
        lua_pushinteger(L, cfg->fontId);
        lua_pushinteger(L, cfg->fontSize);
        lua_pushinteger(L, cfg->letterSpacing);
        lua_pushinteger(L, cfg->lineHeight);
        lua_pushinteger(L, cfg->wrapMode);
        lua_pushinteger(L, cfg->textAlignment);
        lua_pushnumber(L, cfg->textColor.r);
        lua_pushnumber(L, cfg->textColor.g);
        lua_pushnumber(L, cfg->textColor.b);
        lua_pushnumber(L, cfg->textColor.a);
    } else {
        for (int i = 0; i < 10; ++i) lua_pushnil(L);
    }

    Clay_Dimensions out = (Clay_Dimensions){0,0};

    if (lua_pcall(L, 11, 2, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        fprintf(stderr, "[clay] measureText error: %s\n", err ? err : "(unknown)");
        lua_pop(L, 1);
        return out;
    }

    if (lua_isnumber(L, -1))
        out.height = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    if (lua_isnumber(L, -1))
        out.width = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    if (out.width <= 0)  out.width  = 1.0f;
    if (out.height <= 0) out.height = 1.0f;

    return out;
}

static int l_Clay_SetMeasureTextFunction(lua_State *L) {
    if (!lua_isfunction(L, 1) && !lua_isnil(L, 1)) {
        return luaL_error(L, "setMeasureTextFunction(func|nil)");
    }

    g_LuaState = L;
    if (g_MeasureTextRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_MeasureTextRef);
        g_MeasureTextRef = LUA_NOREF;
    }

    if (lua_isfunction(L, 1)) {
        lua_pushvalue(L, 1);
        g_MeasureTextRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    Clay_SetMeasureTextFunction(Bridge_MeasureTextFunction, NULL);
    return 0;
}

static Clay_ElementId clay_check_element_id(lua_State *L, int idx)
{
    Clay_ElementId eid = (Clay_ElementId){0};
    idx = lua_absindex(L, idx);

    if (!lua_istable(L, idx)) {
        luaL_error(L, "expected id table from clay.id()");
    }

    lua_getfield(L, idx, "id");
    eid.id = (uint32_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "offset");
    eid.offset = (uint32_t)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, idx, "baseId");
    eid.baseId = (uint32_t)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, idx, "stringId");
    lua_pop(L, 1);

    return eid;
}

// -----------------------------------------------------------------------------
// Fluent builders API
// -----------------------------------------------------------------------------

typedef struct {
    Clay_ElementDeclaration decl;
    Clay_ElementId elementId;
    Clay_Context *context;
    int active;
    int configured;
    int clipOffsetExplicit;
} LuaClayElementBuilder;

typedef struct {
    LuaClayElementBuilder *element;
    int elementRef;
    int active;
} LuaClayFloatingBuilder;

typedef struct {
    Clay_String text;
    Clay_TextElementConfig cfg;
    Clay_Context *context;
    int active;
} LuaClayTextBuilder;

static void clay_push_element_id_table(lua_State *L, Clay_ElementId eid, Clay_String explicit_sid);

static void text_builder_emit(lua_State *L, LuaClayTextBuilder *t) {
    if (!t || !t->active) return;
    if (Clay_GetCurrentContext() != t->context || g_ClayBuildingContext != t->context) {
        luaL_error(L, "text builder belongs to a different or inactive Clay layout");
    }
    CLAY_TEXT(t->text, t->cfg);
    t->cfg.userData = NULL;
    t->active = 0;
}

static LuaClayElementBuilder* check_elem_builder(lua_State *L, int idx) {
    return (LuaClayElementBuilder*)luaL_checkudata(L, idx, "ClayElementBuilder");
}

static LuaClayTextBuilder* check_text_builder(lua_State *L, int idx) {
    LuaClayTextBuilder *t = (LuaClayTextBuilder*)luaL_checkudata(L, idx, "ClayTextBuilder");
    if (t->active && (Clay_GetCurrentContext() != t->context || g_ClayBuildingContext != t->context)) {
        luaL_error(L, "text builder belongs to a different or inactive Clay layout");
    }
    return t;
}

static void elem_builder_ensure_open(lua_State *L, LuaClayElementBuilder *b) {
    if (!b || !b->active) luaL_error(L, "element builder is not active (already closed?)");
    if (Clay_GetCurrentContext() != b->context || g_ClayBuildingContext != b->context) {
        luaL_error(L, "element builder belongs to a different or inactive Clay layout");
    }
}

static LuaClayFloatingBuilder* check_floating_builder(lua_State *L, int idx) {
    LuaClayFloatingBuilder *f = (LuaClayFloatingBuilder*)luaL_checkudata(L, idx, "ClayFloatingBuilder");
    if (!f->active || !f->element) {
        luaL_error(L, "floating builder is closed (call :floating() before using it)");
    }
    elem_builder_ensure_open(L, f->element);
    return f;
}

static void elem_builder_detach_ptrs(LuaClayElementBuilder *b) {
    b->decl.userData = NULL;
    b->decl.image.imageData = NULL;
    b->decl.custom.customData = NULL;
}

static int clay_theme_call(lua_State *L, const char *method, int aliasIndex, uint32_t elementId, bool hasAlias) {
    int originalTop = lua_gettop(L);
    if (hasAlias) aliasIndex = lua_absindex(L, aliasIndex);
    int optionalPop = strcmp(method, "_popElement") == 0;
    lua_getglobal(L, "clay");
    if (!lua_istable(L, -1)) {
        lua_settop(L, originalTop);
        if (optionalPop) return LUA_OK;
        luaL_error(L, "clay.theme is unavailable; require theme.lua before using :style()");
        return LUA_ERRRUN;
    }
    lua_getfield(L, -1, "theme");
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) {
        lua_settop(L, originalTop);
        if (optionalPop) return LUA_OK;
        luaL_error(L, "clay.theme is unavailable; require theme.lua before using :style()");
        return LUA_ERRRUN;
    }
    lua_getfield(L, -1, method);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, originalTop);
        if (optionalPop) return LUA_OK;
        luaL_error(L, "clay.theme.%s is unavailable", method);
        return LUA_ERRRUN;
    }

    int argumentCount = 1;
    if (hasAlias) {
        lua_pushvalue(L, aliasIndex);
        argumentCount++;
    }
    lua_pushinteger(L, (lua_Integer)elementId);
    int status = lua_pcall(L, argumentCount, 0, 0);
    if (status == LUA_OK) lua_settop(L, originalTop);
    return status;
}

static void clay_close_open_element(lua_State *L, Clay_Context *expectedContext) {
    if (!expectedContext || Clay_GetCurrentContext() != expectedContext ||
        g_ClayBuildingContext != expectedContext) {
        luaL_error(L, "cannot close an element outside its active Clay layout");
    }
    Clay_LayoutElement *element = Clay__GetOpenLayoutElement();
    if (element) {
        int status = clay_theme_call(L, "_popElement", 0, element->id, false);
        if (status != LUA_OK) lua_error(L);
    }
    Clay__CloseElement();
}

static bool clay_theme_read_color(lua_State *L, const char *token, Clay_Color *out) {
    int originalTop = lua_gettop(L);
    bool found = false;
    lua_getglobal(L, "clay");
    if (!lua_istable(L, -1)) goto cleanup;
    lua_getfield(L, -1, "theme");
    if (!lua_istable(L, -1)) goto cleanup;
    lua_getfield(L, -1, "colors");
    if (!lua_istable(L, -1)) goto cleanup;
    lua_getfield(L, -1, token);
    if (lua_istable(L, -1)) found = clay_read_color(L, -1, out) != 0;
cleanup:
    lua_settop(L, originalTop);
    return found;
}

static int l_Elem_style(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    luaL_checktype(L, 2, LUA_TSTRING);
    Clay_LayoutElement *element = Clay__GetOpenLayoutElement();
    if (!element) return luaL_error(L, "element :style() called without an open Clay element");
    int status = clay_theme_call(L, "_push", 2, element->id, true);
    if (status != LUA_OK) return lua_error(L);
    lua_settop(L, 1);
    return 1;
}

static void elem_builder_configure_if_needed(lua_State *L, LuaClayElementBuilder *b) {
    (void)L;
    if (b->configured) return;

    if ((b->decl.clip.horizontal || b->decl.clip.vertical) && !b->clipOffsetExplicit) {
        b->decl.clip.childOffset = Clay_GetScrollOffset();
    }

    Clay__ConfigureOpenElement(CLAY__CONFIG_WRAPPER(Clay_ElementDeclaration, b->decl));
    b->configured = 1;
    elem_builder_detach_ptrs(b);
}

// --- clay.element(...) ---
static Clay_ElementId clay_element_id_from_args(lua_State *L, int arg1) {
    // Anonymous elements derive an ID from the current parent and child position.
    if (lua_gettop(L) < arg1) {
        Clay_Context *ctx = Clay_GetCurrentContext();
        if (!ctx) luaL_error(L, "Clay context is null (did you call clay.initialize()?)");
        if (g_ClayBuildingContext != ctx) {
            luaL_error(L, "clay.element() must be called between beginLayout() and endLayoutFast()");
        }

        Clay_LayoutElement *parent = clay_current_open_element(ctx);

        Clay_ElementId eid;
        uint32_t parentId = parent ? parent->id : 0;
        uint32_t idParent = parent ? parent->id : clay_auto_root_parent_id();
        uint32_t offset = parent
            ? (uint32_t)(parent->children.length + parent->floatingChildrenCount)
            : (uint32_t)ctx->layoutElementTreeRoots.length;
        if (g_ClayKeyScope && g_ClayKeyScope->context == ctx &&
            g_ClayKeyScope->parentId == parentId) {
            idParent = g_ClayKeyScope->keyParentId;
            offset = g_ClayKeyScope->nextOffset++;
        }

        eid = Clay__HashNumber(offset, idParent);

        return eid;
    }

    int t = lua_type(L, arg1);

    if (t == LUA_TTABLE) {
        return clay_check_element_id(L, arg1);
    }

    if (t == LUA_TSTRING) {
        Clay_String s = Clay_BorrowLuaString(L, arg1);
        uint32_t index = (uint32_t)luaL_optinteger(L, arg1 + 1, 0);
        bool isLocal = lua_toboolean(L, arg1 + 2);

        Clay_ElementId eid;
        if (index > 0) {
            eid = Clay__HashStringWithOffset(s, index, isLocal ? Clay__GetParentElementId() : 0);
        } else {
            eid = Clay__HashString(s, isLocal ? Clay__GetParentElementId() : 0);
        }
        return eid;
    }

    luaL_error(L, "clay.element expects (), (name[, index[, isLocal]]), or (idTable)");
    return (Clay_ElementId){0};
}

static int l_Clay_ElementBuilder_New(lua_State *L) {
    Clay_Context *context = Clay_GetCurrentContext();
    if (!context || g_ClayBuildingContext != context) {
        return luaL_error(L, "clay.element() must be called between beginLayout() and endLayoutFast()");
    }
    Clay_ElementId eid = clay_element_id_from_args(L, 1);

    LuaClayElementBuilder *b = (LuaClayElementBuilder*)lua_newuserdata(L, sizeof(LuaClayElementBuilder));
    memset(b, 0, sizeof(*b));
    b->decl = (Clay_ElementDeclaration){0};
    b->elementId = eid;
    b->context = Clay_GetCurrentContext();
    b->decl.layout = CLAY_LAYOUT_DEFAULT;
    b->active = 1;
    b->configured = 0;
    b->clipOffsetExplicit = 0;

    Clay__OpenElementWithId(eid);

    luaL_setmetatable(L, "ClayElementBuilder");
    return 1;
}

// --- ElementBuilder setters (return self) ---

// --- ElementBuilder:open() ---
// Finalizes configuration but does NOT close the element.
// After calling open(), the builder becomes inactive so it won't auto-close on GC.
// You must later call clay.close() to close it.
static int l_Elem_open(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);

    // Ensure element exists
    elem_builder_ensure_open(L, b);

    // Finalize configuration (same as close would do)
    elem_builder_configure_if_needed(L, b);

    // Mark inactive so __gc does not auto-close it
    b->active = 0;

    return 0;
}

static int l_Elem_alias(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    size_t len = 0;
    const char *name = luaL_checklstring(L, 2, &len);
    if (len == 0) return luaL_argerror(L, 2, "alias must not be empty");
    if (g_ClayAliasRegistrarRef == LUA_NOREF) {
        return luaL_error(L, ":alias() is available while building a retained root");
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_ClayAliasRegistrarRef);
    lua_pushlstring(L, name, len);
    clay_push_element_id_table(L, b->elementId, (Clay_String){0});
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) return lua_error(L);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_id(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    clay_push_element_id_table(L, b->elementId, (Clay_String){0});
    return 1;
}

static int l_Elem_layoutDirection(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    b->decl.layout.layoutDirection = (Clay_LayoutDirection)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_childGap(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }
    b->decl.layout.childGap = (uint16_t)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_childAlignment(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    b->decl.layout.childAlignment.x = (Clay_LayoutAlignmentX)luaL_checkinteger(L, 2);
    b->decl.layout.childAlignment.y = (Clay_LayoutAlignmentY)luaL_checkinteger(L, 3);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_padding(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);

    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }

    int n = lua_gettop(L) - 1;
    if (n == 1) {
        float all = (float)luaL_checknumber(L, 2);
        b->decl.layout.padding.left = (uint16_t)all;
        b->decl.layout.padding.right = (uint16_t)all;
        b->decl.layout.padding.top = (uint16_t)all;
        b->decl.layout.padding.bottom = (uint16_t)all;
    } else if (n == 2) {
        float x = (float)luaL_checknumber(L, 2);
        float y = (float)luaL_checknumber(L, 3);
        b->decl.layout.padding.left = (uint16_t)x;
        b->decl.layout.padding.right = (uint16_t)x;
        b->decl.layout.padding.top = (uint16_t)y;
        b->decl.layout.padding.bottom = (uint16_t)y;
    } else if (n == 4) {
        float l = (float)luaL_checknumber(L, 2);
        float t = (float)luaL_checknumber(L, 3);
        float r = (float)luaL_checknumber(L, 4);
        float bt = (float)luaL_checknumber(L, 5);
        b->decl.layout.padding.left = (uint16_t)l;
        b->decl.layout.padding.top = (uint16_t)t;
        b->decl.layout.padding.right = (uint16_t)r;
        b->decl.layout.padding.bottom = (uint16_t)bt;
    } else {
        return luaL_error(L, "padding(all) | padding(x,y) | padding(l,t,r,b)");
    }
    lua_settop(L, 1);
    return 1;
}

static void set_sizing_axis(lua_State *L, Clay_SizingAxis *axis, Clay__SizingType type, int start_arg, int n_args) {
    axis->type = type;
    if (type == CLAY__SIZING_TYPE_FIXED) {
        if (n_args != 1) luaL_error(L, "FIXED expects 1 argument (size)");
        float size = (float)luaL_checknumber(L, start_arg);
        axis->size.minMax.min = size;
        axis->size.minMax.max = size;
        return;
    }
    if (type == CLAY__SIZING_TYPE_PERCENT) {
        if (n_args != 1) luaL_error(L, "PERCENT expects 1 argument (percent)");
        axis->size.percent = (float)luaL_checknumber(L, start_arg);
        return;
    }
    if (n_args == 0) {
        axis->size.minMax.min = 0.0f;
        axis->size.minMax.max = 0.0f;
    } else if (n_args == 1) {
        axis->size.minMax.min = (float)luaL_checknumber(L, start_arg);
        axis->size.minMax.max = 0.0f;
    } else if (n_args == 2) {
        axis->size.minMax.min = (float)luaL_checknumber(L, start_arg);
        axis->size.minMax.max = (float)luaL_checknumber(L, start_arg + 1);
    } else {
        luaL_error(L, "GROW/FIT expects 0, 1, or 2 arguments (min[, max])");
    }
}

static int l_Elem_width(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    Clay__SizingType t = (Clay__SizingType)luaL_checkinteger(L, 2);
    int n = lua_gettop(L) - 2;
    set_sizing_axis(L, &b->decl.layout.sizing.width, t, 3, n);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_height(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    Clay__SizingType t = (Clay__SizingType)luaL_checkinteger(L, 2);
    int n = lua_gettop(L) - 2;
    set_sizing_axis(L, &b->decl.layout.sizing.height, t, 3, n);
    lua_settop(L, 1);
    return 1;
}

static void elem_set_fixed_size_axis(lua_State *L, Clay_SizingAxis *axis, int arg) {
    if (lua_isnoneornil(L, arg)) return;
    float size = (float)luaL_checknumber(L, arg);
    axis->type = CLAY__SIZING_TYPE_FIXED;
    axis->size.minMax.min = size;
    axis->size.minMax.max = size;
}

static void elem_set_range_size_axis(lua_State *L, Clay_SizingAxis *axis,
                                     Clay__SizingType type, int min_arg, int max_arg) {
    axis->type = type;
    axis->size.minMax.min = lua_isnoneornil(L, min_arg)
        ? 0.0f : (float)luaL_checknumber(L, min_arg);
    axis->size.minMax.max = lua_isnoneornil(L, max_arg)
        ? 0.0f : (float)luaL_checknumber(L, max_arg);
}

static int l_Elem_size(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    if (lua_gettop(L) > 3) return luaL_error(L, "size(width[, height])");
    elem_set_fixed_size_axis(L, &b->decl.layout.sizing.width, 2);
    elem_set_fixed_size_axis(L, &b->decl.layout.sizing.height, 3);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_growSize(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    if (lua_gettop(L) > 5)
        return luaL_error(L, "growSize(widthMin, widthMax, heightMin, heightMax)");
    elem_set_range_size_axis(L, &b->decl.layout.sizing.width, CLAY__SIZING_TYPE_GROW, 2, 3);
    elem_set_range_size_axis(L, &b->decl.layout.sizing.height, CLAY__SIZING_TYPE_GROW, 4, 5);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_fitSize(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    if (lua_gettop(L) > 5)
        return luaL_error(L, "fitSize(widthMin, widthMax, heightMin, heightMax)");
    elem_set_range_size_axis(L, &b->decl.layout.sizing.width, CLAY__SIZING_TYPE_FIT, 2, 3);
    elem_set_range_size_axis(L, &b->decl.layout.sizing.height, CLAY__SIZING_TYPE_FIT, 4, 5);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_percentSize(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    if (lua_gettop(L) > 3) return luaL_error(L, "percentSize(width[, height])");
    if (!lua_isnoneornil(L, 2)) {
        b->decl.layout.sizing.width.type = CLAY__SIZING_TYPE_PERCENT;
        b->decl.layout.sizing.width.size.percent = (float)luaL_checknumber(L, 2);
    }
    if (!lua_isnoneornil(L, 3)) {
        b->decl.layout.sizing.height.type = CLAY__SIZING_TYPE_PERCENT;
        b->decl.layout.sizing.height.size.percent = (float)luaL_checknumber(L, 3);
    }
    lua_settop(L, 1);
    return 1;
}

/* ----- sugar: bg/border and sizing/align shorthands ----- */
static int l_Elem_bg(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);

    Clay_Color c = (Clay_Color){0,0,0,0};
    if (!clay_read_color(L, 2, &c)) { lua_settop(L,1); return 1; }
    b->decl.backgroundColor = c;
    lua_settop(L,1);
    return 1;
}

static int l_Elem_border(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);

    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }

    Clay_Color c = (Clay_Color){0,0,0,255};
    int top = lua_gettop(L);

    // Handle packed + width: border(0xRRGGBBAA, width)
    if (lua_isnumber(L,2) && top == 3) {
        uint32_t hex = (uint32_t)lua_tointeger(L,2);
        c.r = (float)((hex >> 24) & 0xFF);
        c.g = (float)((hex >> 16) & 0xFF);
        c.b = (float)((hex >> 8) & 0xFF);
        c.a = (float)(hex & 0xFF);
        b->decl.border.color = c;

        uint16_t w = (uint16_t)luaL_checkinteger(L,3);
        b->decl.border.width.left = w;
        b->decl.border.width.right = w;
        b->decl.border.width.top = w;
        b->decl.border.width.bottom = w;
        lua_settop(L,1);
        return 1;
    }

    // Table + width: border({..}, width)
    if (lua_istable(L,2) && top >= 3 && lua_isnumber(L,3)) {
        clay_read_color(L,2,&c);
        b->decl.border.color = c;

        uint16_t w = (uint16_t)lua_tointeger(L,3);
        b->decl.border.width.left = w;
        b->decl.border.width.right = w;
        b->decl.border.width.top = w;
        b->decl.border.width.bottom = w;
        lua_settop(L,1);
        return 1;
    }

    // Otherwise: just set border color from whatever form.
    // (If you also want width with rgba numbers, call :borderWidth(...))
    clay_read_color(L, 2, &c);
    b->decl.border.color = c;

    lua_settop(L,1);
    return 1;
}

static Clay_FloatingAttachPointType clay_parse_attach_point(lua_State *L, int idx) {
    if (lua_isnumber(L, idx)) {
        int v = (int)lua_tointeger(L, idx);
        if (v < 0 || v > 8) luaL_error(L, "attach point out of range (0..8)");
        return (Clay_FloatingAttachPointType)v;
    }
    const char *s = luaL_checkstring(L, idx);
    // 2-char codes
    if (s[0]=='l'&&s[1]=='t') return CLAY_ATTACH_POINT_LEFT_TOP;
    if (s[0]=='l'&&s[1]=='c') return CLAY_ATTACH_POINT_LEFT_CENTER;
    if (s[0]=='l'&&s[1]=='b') return CLAY_ATTACH_POINT_LEFT_BOTTOM;
    if (s[0]=='c'&&s[1]=='t') return CLAY_ATTACH_POINT_CENTER_TOP;
    if (s[0]=='c'&&s[1]=='c') return CLAY_ATTACH_POINT_CENTER_CENTER;
    if (s[0]=='c'&&s[1]=='b') return CLAY_ATTACH_POINT_CENTER_BOTTOM;
    if (s[0]=='r'&&s[1]=='t') return CLAY_ATTACH_POINT_RIGHT_TOP;
    if (s[0]=='r'&&s[1]=='c') return CLAY_ATTACH_POINT_RIGHT_CENTER;
    if (s[0]=='r'&&s[1]=='b') return CLAY_ATTACH_POINT_RIGHT_BOTTOM;
    luaL_error(L, "unknown attach point '%s' (use lt/lc/lb/ct/cc/cb/rt/rc/rb)", s);
    return CLAY_ATTACH_POINT_LEFT_TOP;
}

static int l_Elem_row(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    b->decl.layout.layoutDirection = CLAY_LEFT_TO_RIGHT;
    lua_settop(L,1);
    return 1;
}

static int l_Elem_col(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    b->decl.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    lua_settop(L,1);
    return 1;
}

static int l_Elem_gap(lua_State *L) {
    return l_Elem_childGap(L);
}

static int l_Elem_align(lua_State *L) {
    return l_Elem_childAlignment(L);
}

// 9-grid alignment helpers
static int l_Elem_alignXY(lua_State *L, Clay_LayoutAlignmentX ax, Clay_LayoutAlignmentY ay) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    b->decl.layout.childAlignment.x = ax;
    b->decl.layout.childAlignment.y = ay;
    lua_settop(L,1);
    return 1;
}

#define CLAY_ALIGN_METHOD(NAME, AX, AY) \
static int l_Elem_align##NAME(lua_State *L){ return l_Elem_alignXY(L, AX, AY); }

CLAY_ALIGN_METHOD(LT, CLAY_ALIGN_X_LEFT,   CLAY_ALIGN_Y_TOP)
CLAY_ALIGN_METHOD(CT, CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_TOP)
CLAY_ALIGN_METHOD(RT, CLAY_ALIGN_X_RIGHT,  CLAY_ALIGN_Y_TOP)

CLAY_ALIGN_METHOD(LC, CLAY_ALIGN_X_LEFT,   CLAY_ALIGN_Y_CENTER)
CLAY_ALIGN_METHOD(CC, CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER)
CLAY_ALIGN_METHOD(RC, CLAY_ALIGN_X_RIGHT,  CLAY_ALIGN_Y_CENTER)

CLAY_ALIGN_METHOD(LB, CLAY_ALIGN_X_LEFT,   CLAY_ALIGN_Y_BOTTOM)
CLAY_ALIGN_METHOD(CB, CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_BOTTOM)
CLAY_ALIGN_METHOD(RB, CLAY_ALIGN_X_RIGHT,  CLAY_ALIGN_Y_BOTTOM)

// sizing shorthands
static int l_Elem_w(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }
    float v = (float)luaL_checknumber(L,2);
    b->decl.layout.sizing.width.type = CLAY__SIZING_TYPE_FIXED;
    b->decl.layout.sizing.width.size.minMax.min = v;
    b->decl.layout.sizing.width.size.minMax.max = v;
    lua_settop(L,1);
    return 1;
}

static int l_Elem_h(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }
    float v = (float)luaL_checknumber(L,2);
    b->decl.layout.sizing.height.type = CLAY__SIZING_TYPE_FIXED;
    b->decl.layout.sizing.height.size.minMax.min = v;
    b->decl.layout.sizing.height.size.minMax.max = v;
    lua_settop(L,1);
    return 1;
}

static int l_Elem_growW(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    b->decl.layout.sizing.width.type = CLAY__SIZING_TYPE_GROW;

    int n = lua_gettop(L) - 1;
    if (n >= 1 && !lua_isnil(L,2)) b->decl.layout.sizing.width.size.minMax.min = (float)luaL_optnumber(L,2,0.0f);
    if (n >= 2 && !lua_isnil(L,3)) b->decl.layout.sizing.width.size.minMax.max = (float)luaL_optnumber(L,3,0.0f);

    lua_settop(L,1);
    return 1;
}

static int l_Elem_growH(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    b->decl.layout.sizing.height.type = CLAY__SIZING_TYPE_GROW;

    int n = lua_gettop(L) - 1;
    if (n >= 1 && !lua_isnil(L,2)) b->decl.layout.sizing.height.size.minMax.min = (float)luaL_optnumber(L,2,0.0f);
    if (n >= 2 && !lua_isnil(L,3)) b->decl.layout.sizing.height.size.minMax.max = (float)luaL_optnumber(L,3,0.0f);

    lua_settop(L,1);
    return 1;
}

static int l_Elem_fitW(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    b->decl.layout.sizing.width.type = CLAY__SIZING_TYPE_FIT;

    int n = lua_gettop(L) - 1;
    if (n >= 1 && !lua_isnil(L,2)) b->decl.layout.sizing.width.size.minMax.min = (float)luaL_optnumber(L,2,0.0f);
    if (n >= 2 && !lua_isnil(L,3)) b->decl.layout.sizing.width.size.minMax.max = (float)luaL_optnumber(L,3,0.0f);

    lua_settop(L,1);
    return 1;
}

static int l_Elem_fitH(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    b->decl.layout.sizing.height.type = CLAY__SIZING_TYPE_FIT;

    int n = lua_gettop(L) - 1;
    if (n >= 1 && !lua_isnil(L,2)) b->decl.layout.sizing.height.size.minMax.min = (float)luaL_optnumber(L,2,0.0f);
    if (n >= 2 && !lua_isnil(L,3)) b->decl.layout.sizing.height.size.minMax.max = (float)luaL_optnumber(L,3,0.0f);

    lua_settop(L,1);
    return 1;
}

static int l_Elem_wPct(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }
    b->decl.layout.sizing.width.type = CLAY__SIZING_TYPE_PERCENT;
    b->decl.layout.sizing.width.size.percent = (float)luaL_checknumber(L,2);
    lua_settop(L,1);
    return 1;
}

static int l_Elem_hPct(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L,1);
    elem_builder_ensure_open(L,b);
    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }
    b->decl.layout.sizing.height.type = CLAY__SIZING_TYPE_PERCENT;
    b->decl.layout.sizing.height.size.percent = (float)luaL_checknumber(L,2);
    lua_settop(L,1);
    return 1;
}

static Clay_TextElementConfigWrapMode clay_parse_wrap_mode(lua_State *L, int idx, Clay_TextElementConfigWrapMode fallback) {
    idx = lua_absindex(L, idx);
    if (lua_isnil(L, idx)) return fallback;

    if (lua_isnumber(L, idx)) {
        int v = (int)lua_tointeger(L, idx);
        // allow 0/1/2 shorthand too (optional)
        if (v == 0) return CLAY_TEXT_WRAP_NONE;
        if (v == 1) return CLAY_TEXT_WRAP_WORDS;
        if (v == 2) return CLAY_TEXT_WRAP_NEWLINES;
        return (Clay_TextElementConfigWrapMode)v;
    }

    if (lua_isstring(L, idx)) {
        size_t len = 0;
        const char *s = lua_tolstring(L, idx, &len);
        if (!s || len == 0) return fallback;

        // shorthand
        if (strcmp(s, "none") == 0 || strcmp(s, "N") == 0 || strcmp(s, "n") == 0)
            return CLAY_TEXT_WRAP_NONE;
        if (strcmp(s, "words") == 0 || strcmp(s, "W") == 0 || strcmp(s, "w") == 0)
            return CLAY_TEXT_WRAP_WORDS;
        if (strcmp(s, "newlines") == 0 || strcmp(s, "nl") == 0 || strcmp(s, "NL") == 0)
            return CLAY_TEXT_WRAP_NEWLINES;
    }

    return fallback;
}

static Clay_TextAlignment clay_parse_text_alignment(lua_State *L, int idx, Clay_TextAlignment fallback) {
    idx = lua_absindex(L, idx);
    if (lua_isnil(L, idx)) return fallback;

    if (lua_isnumber(L, idx)) {
        int v = (int)lua_tointeger(L, idx);
        // Allow both direct Clay constants and 0/1/2 shorthand.
        if (v == 0) return CLAY_TEXT_ALIGN_LEFT;
        if (v == 1) return CLAY_TEXT_ALIGN_CENTER;
        if (v == 2) return CLAY_TEXT_ALIGN_RIGHT;
        return (Clay_TextAlignment)v;
    }

    if (lua_isstring(L, idx)) {
        size_t len = 0;
        const char *s = lua_tolstring(L, idx, &len);
        if (!s || len == 0) return fallback;

        // First char shorthand
        char c = s[0];
        if (c == 'l' || c == 'L') return CLAY_TEXT_ALIGN_LEFT;
        if (c == 'c' || c == 'C') return CLAY_TEXT_ALIGN_CENTER;
        if (c == 'r' || c == 'R') return CLAY_TEXT_ALIGN_RIGHT;

        // Words
        if (strcmp(s, "left") == 0) return CLAY_TEXT_ALIGN_LEFT;
        if (strcmp(s, "center") == 0) return CLAY_TEXT_ALIGN_CENTER;
        if (strcmp(s, "right") == 0) return CLAY_TEXT_ALIGN_RIGHT;
    }

    return fallback;
}

static int l_Text_wrap(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    t->cfg.wrapMode = clay_parse_wrap_mode(L, 2, t->cfg.wrapMode);
    lua_settop(L, 1);
    return 1;
}

static int l_Text_align(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    t->cfg.textAlignment = clay_parse_text_alignment(L, 2, t->cfg.textAlignment);
    lua_settop(L, 1);
    return 1;
}

/* ----- existing explicit setters (kept) ----- */
static int l_Elem_backgroundColor(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    b->decl.backgroundColor.r = (float)luaL_checknumber(L, 2);
    b->decl.backgroundColor.g = (float)luaL_checknumber(L, 3);
    b->decl.backgroundColor.b = (float)luaL_checknumber(L, 4);
    b->decl.backgroundColor.a = (float)luaL_optnumber(L, 5, 255.0);
    lua_settop(L, 1);
    return 1;
}

static Clay_TransitionProperty clay_transition_property_from_name(lua_State *L, int idx) {
    const char *name = luaL_checkstring(L, idx);
    if (strcmp(name, "x") == 0) return CLAY_TRANSITION_PROPERTY_X;
    if (strcmp(name, "y") == 0) return CLAY_TRANSITION_PROPERTY_Y;
    if (strcmp(name, "position") == 0) return CLAY_TRANSITION_PROPERTY_POSITION;
    if (strcmp(name, "width") == 0) return CLAY_TRANSITION_PROPERTY_WIDTH;
    if (strcmp(name, "height") == 0) return CLAY_TRANSITION_PROPERTY_HEIGHT;
    if (strcmp(name, "dimensions") == 0) return CLAY_TRANSITION_PROPERTY_DIMENSIONS;
    if (strcmp(name, "boundingBox") == 0) return CLAY_TRANSITION_PROPERTY_BOUNDING_BOX;
    if (strcmp(name, "backgroundColor") == 0) return CLAY_TRANSITION_PROPERTY_BACKGROUND_COLOR;
    if (strcmp(name, "overlayColor") == 0) return CLAY_TRANSITION_PROPERTY_OVERLAY_COLOR;
    if (strcmp(name, "borderColor") == 0) return CLAY_TRANSITION_PROPERTY_BORDER_COLOR;
    if (strcmp(name, "borderWidth") == 0) return CLAY_TRANSITION_PROPERTY_BORDER_WIDTH;
    if (strcmp(name, "border") == 0) return CLAY_TRANSITION_PROPERTY_BORDER;
    luaL_error(L, "unknown transition property '%s'", name);
    return CLAY_TRANSITION_PROPERTY_NONE;
}

static int l_Elem_transition(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    if (lua_gettop(L) < 3) {
        return luaL_error(L, "transition(duration, property, ...)");
    }

    float duration = (float)luaL_checknumber(L, 2);
    if (duration < 0) return luaL_argerror(L, 2, "duration must be non-negative");

    Clay_TransitionProperty properties = CLAY_TRANSITION_PROPERTY_NONE;
    for (int i = 3; i <= lua_gettop(L); ++i) {
        properties = (Clay_TransitionProperty)(properties | clay_transition_property_from_name(L, i));
    }
    if (properties == CLAY_TRANSITION_PROPERTY_NONE) {
        return luaL_error(L, "transition requires at least one property");
    }

    b->decl.transition.handler = Clay_EaseOut;
    b->decl.transition.duration = duration;
    b->decl.transition.properties = properties;
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_cornerRadius(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);

    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }

    int n = lua_gettop(L) - 1;
    if (n == 1) {
        float r = (float)luaL_checknumber(L, 2);
        b->decl.cornerRadius.topLeft = r;
        b->decl.cornerRadius.topRight = r;
        b->decl.cornerRadius.bottomLeft = r;
        b->decl.cornerRadius.bottomRight = r;
    } else if (n == 4) {
        b->decl.cornerRadius.topLeft = (float)luaL_checknumber(L, 2);
        b->decl.cornerRadius.topRight = (float)luaL_checknumber(L, 3);
        b->decl.cornerRadius.bottomLeft = (float)luaL_checknumber(L, 4);
        b->decl.cornerRadius.bottomRight = (float)luaL_checknumber(L, 5);
    } else {
        return luaL_error(L, "cornerRadius(all) | cornerRadius(tl,tr,bl,br)");
    }
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_borderColor(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    b->decl.border.color.r = (float)luaL_checknumber(L, 2);
    b->decl.border.color.g = (float)luaL_checknumber(L, 3);
    b->decl.border.color.b = (float)luaL_checknumber(L, 4);
    b->decl.border.color.a = (float)luaL_optnumber(L, 5, 255.0);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_borderWidth(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);

    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }

    int n = lua_gettop(L) - 1;
    if (n == 1) {
        uint16_t w = (uint16_t)luaL_checkinteger(L, 2);
        b->decl.border.width.left = w;
        b->decl.border.width.right = w;
        b->decl.border.width.top = w;
        b->decl.border.width.bottom = w;
    } else if (n == 4) {
        b->decl.border.width.left = (uint16_t)luaL_checkinteger(L, 2);
        b->decl.border.width.top = (uint16_t)luaL_checkinteger(L, 3);
        b->decl.border.width.right = (uint16_t)luaL_checkinteger(L, 4);
        b->decl.border.width.bottom = (uint16_t)luaL_checkinteger(L, 5);
    } else {
        return luaL_error(L, "borderWidth(all) | borderWidth(l,t,r,b)");
    }
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_scrollable(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);

    int n = lua_gettop(L) - 1;

    // scrollable(true) => both
    // scrollable(vert, hori)
    if (n <= 0 || lua_isnil(L, 2)) {
        // no args => default both true
        b->decl.clip.vertical = true;
        b->decl.clip.horizontal = true;
    } else if (n == 1) {
        bool v = lua_toboolean(L, 2);
        b->decl.clip.vertical = v;
        b->decl.clip.horizontal = v;
    } else {
        b->decl.clip.vertical = lua_toboolean(L, 2);
        b->decl.clip.horizontal = lua_toboolean(L, 3);
    }

    lua_settop(L, 1);
    return 1;
}

static int l_Elem_clip(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    int n = lua_gettop(L) - 1;
    if (n == 1) {
        bool v = lua_toboolean(L, 2);
        b->decl.clip.horizontal = v;
        b->decl.clip.vertical = v;
    } else {
        b->decl.clip.horizontal = lua_toboolean(L, 2);
        b->decl.clip.vertical = lua_toboolean(L, 3);
    }
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_clipHorizontal(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    b->decl.clip.horizontal = lua_toboolean(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_clipVertical(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    b->decl.clip.vertical = lua_toboolean(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_childOffset(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    b->decl.clip.childOffset.x = (float)luaL_checknumber(L, 2);
    b->decl.clip.childOffset.y = (float)luaL_checknumber(L, 3);
    b->clipOffsetExplicit = 1;
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_aspectRatio(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    b->decl.aspectRatio.aspectRatio = (float)luaL_checknumber(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_imageData(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    clay_set_ptr_from_lua(L, 2, &b->decl.image.imageData);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_customData(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    clay_set_ptr_from_lua(L, 2, &b->decl.custom.customData);
    lua_settop(L, 1);
    return 1;
}

static int l_Elem_userData(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    clay_set_ptr_from_lua(L, 2, &b->decl.userData);
    lua_settop(L, 1);
    return 1;
}

// :floating() opens a scoped builder for the fields in Clay_FloatingElementConfig.
// The proxy keeps its owning element builder alive until :done() or garbage collection.
static int l_Elem_floating(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);

    LuaClayFloatingBuilder *f = (LuaClayFloatingBuilder*)lua_newuserdata(L, sizeof(*f));
    f->element = b;
    f->active = 1;
    lua_pushvalue(L, 1);
    f->elementRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // A floating element must declare what it floats against. Parent is the
    // useful default for a nested overlay; :root() and :to(id) override it.
    b->decl.floating.attachTo = CLAY_ATTACH_TO_PARENT;

    luaL_setmetatable(L, "ClayFloatingBuilder");
    return 1;
}

static int l_Floating_parent(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    f->element->decl.floating.attachTo = CLAY_ATTACH_TO_PARENT;
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_root(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    f->element->decl.floating.attachTo = CLAY_ATTACH_TO_ROOT;
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_target(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    if (lua_istable(L, 2)) {
        Clay_ElementId eid = clay_check_element_id(L, 2);
        f->element->decl.floating.parentId = eid.id;
    } else {
        f->element->decl.floating.parentId = (uint32_t)luaL_checkinteger(L, 2);
    }
    f->element->decl.floating.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID;
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_attach(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    f->element->decl.floating.attachPoints.element = clay_parse_attach_point(L, 2);
    f->element->decl.floating.attachPoints.parent = clay_parse_attach_point(L, 3);
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_offset(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    f->element->decl.floating.offset.x = (float)luaL_checknumber(L, 2);
    f->element->decl.floating.offset.y = (float)luaL_checknumber(L, 3);
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_expand(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    f->element->decl.floating.expand.width = (float)luaL_checknumber(L, 2);
    f->element->decl.floating.expand.height = (float)luaL_checknumber(L, 3);
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_zIndex(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    f->element->decl.floating.zIndex = (int16_t)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_pointerCapture(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    int on = lua_isnoneornil(L, 2) ? 1 : lua_toboolean(L, 2);
    f->element->decl.floating.pointerCaptureMode = on
        ? CLAY_POINTER_CAPTURE_MODE_CAPTURE
        : CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_pointerCaptureMode(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    f->element->decl.floating.pointerCaptureMode = (Clay_PointerCaptureMode)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_clipTo(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    f->element->decl.floating.clipTo = (Clay_FloatingClipToElement)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_clipToParent(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    int on = lua_isnoneornil(L, 2) ? 1 : lua_toboolean(L, 2);
    f->element->decl.floating.clipTo = on
        ? CLAY_CLIP_TO_ATTACHED_PARENT
        : CLAY_CLIP_TO_NONE;
    lua_settop(L, 1);
    return 1;
}

static int l_Floating_done(lua_State *L) {
    LuaClayFloatingBuilder *f = check_floating_builder(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, f->elementRef);
    luaL_unref(L, LUA_REGISTRYINDEX, f->elementRef);
    f->elementRef = LUA_NOREF;
    f->element = NULL;
    f->active = 0;
    return 1;
}

static int l_Floating_gc(lua_State *L) {
    LuaClayFloatingBuilder *f = (LuaClayFloatingBuilder*)luaL_testudata(L, 1, "ClayFloatingBuilder");
    if (!f) return 0;
    if (f->active && f->elementRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, f->elementRef);
    }
    f->elementRef = LUA_NOREF;
    f->element = NULL;
    f->active = 0;
    return 0;
}

// --- ElementBuilder:close() ---
static int l_Elem_close(lua_State *L) {
    LuaClayElementBuilder *b = check_elem_builder(L, 1);
    elem_builder_ensure_open(L, b);
    elem_builder_configure_if_needed(L, b);
    clay_close_open_element(L, b->context);
    b->active = 0;
    return 0;
}

static int l_Elem_gc(lua_State *L) {
    LuaClayElementBuilder *b = (LuaClayElementBuilder*)luaL_testudata(L, 1, "ClayElementBuilder");
    if (!b) return 0;
    if (b->active) {
        if (Clay_GetCurrentContext() == b->context && g_ClayBuildingContext == b->context) {
            elem_builder_configure_if_needed(L, b);
            clay_close_open_element(L, b->context);
            b->active = 0;
        } else {
            clay_unref_tagged(L, &b->decl.userData);
            clay_unref_tagged(L, &b->decl.image.imageData);
            clay_unref_tagged(L, &b->decl.custom.customData);
            elem_builder_detach_ptrs(b);
            b->active = 0;
        }
    }
    return 0;
}

static int clay_is_alignment_token(lua_State *L, int idx) {
    idx = lua_absindex(L, idx);
    if (!lua_isstring(L, idx)) return 0;

    const char *s = lua_tostring(L, idx);
    if (!s || !s[0]) return 0;

    // single letter
    if ((s[0]=='L'||s[0]=='l'||s[0]=='C'||s[0]=='c'||s[0]=='R'||s[0]=='r') && s[1]==0)
        return 1;

    // words
    return (strcmp(s,"left")==0 || strcmp(s,"center")==0 || strcmp(s,"right")==0);
}

static int clay_is_wrap_token(lua_State *L, int idx) {
    idx = lua_absindex(L, idx);
    if (!lua_isstring(L, idx)) return 0;
    const char *s = lua_tostring(L, idx);
    if (!s || !s[0]) return 0;
    return (strcmp(s,"none")==0 || strcmp(s,"words")==0 || strcmp(s,"newlines")==0 ||
            strcmp(s,"n")==0 || strcmp(s,"w")==0 || strcmp(s,"nl")==0 ||
            strcmp(s,"N")==0 || strcmp(s,"W")==0 || strcmp(s,"NL")==0);
}

// --- Text Builder ---
static int l_Clay_TextBuilder_New(lua_State *L) {
    int top = lua_gettop(L);

    LuaClayTextBuilder *t = (LuaClayTextBuilder*)lua_newuserdata(L, sizeof(LuaClayTextBuilder));
    memset(t, 0, sizeof(*t));
    t->text = Clay_CopyLuaString(L, 1);
    t->context = Clay_GetCurrentContext();

    t->cfg = (Clay_TextElementConfig){0};
    t->cfg.fontId = 1;
    t->cfg.fontSize = 16;
    t->cfg.textColor = (Clay_Color){255,255,255,255};
    clay_theme_read_color(L, "text", &t->cfg.textColor);
    t->cfg.wrapMode = CLAY_TEXT_WRAP_WORDS;
    t->cfg.textAlignment = CLAY_TEXT_ALIGN_LEFT;
    t->cfg.letterSpacing = 0;
    t->cfg.lineHeight = 0;
    t->cfg.userData = NULL;

    t->active = 1;
    luaL_setmetatable(L, "ClayTextBuilder");

    // Existing callback form: clay.text("label", function(t) ... end)
    if (top >= 2 && lua_isfunction(L, 2)) {
        lua_getglobal(L, "debug");
        lua_getfield(L, -1, "traceback");
        lua_remove(L, -2);
        int traceback_index = lua_gettop(L);

        lua_pushvalue(L, 2);
        lua_pushvalue(L, -3); // builder

        if (lua_pcall(L, 1, 0, traceback_index) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_pop(L, 1);
            return luaL_error(L, "text() config callback failed:\n%s", err ? err : "(unknown)");
        }

        lua_pop(L, 1);
        text_builder_emit(L, t);
        return 0;
    }

    // Overload: clay.text(label, fontId?, fontSize?, color?, alignOrWrap?, wrapOrAlign?)
    if (top >= 2 && !lua_isnil(L, 2)) t->cfg.fontId = (uint16_t)luaL_checkinteger(L, 2);
    if (top >= 3 && !lua_isnil(L, 3)) t->cfg.fontSize = (uint16_t)luaL_checkinteger(L, 3);

    if (top >= 4 && !lua_isnil(L, 4)) {
        Clay_Color c = t->cfg.textColor;
        if (lua_isnumber(L, 4)) {
            uint32_t hex = (uint32_t)lua_tointeger(L, 4);
            c.r = (float)((hex >> 24) & 0xFF);
            c.g = (float)((hex >> 16) & 0xFF);
            c.b = (float)((hex >> 8) & 0xFF);
            c.a = (float)(hex & 0xFF);
        } else if (lua_istable(L, 4)) {
            clay_read_color(L, 4, &c);
        } else {
            return luaL_error(L, "expected color as nil|table|number(s)");
        }
        t->cfg.textColor = c;
    }

    // defaults you already set above:
    // t->cfg.wrapMode = CLAY_TEXT_WRAP_WORDS;
    // t->cfg.textAlignment = CLAY_TEXT_ALIGN_LEFT;

    // Arg 5/6 can be either wrap or alignment, in any order.
    for (int i = 5; i <= 6; i++) {
        if (top < i || lua_isnil(L, i)) continue;

        if (clay_is_alignment_token(L, i) || lua_isnumber(L, i)) {
            // numbers could be ambiguous; we treat as alignment *unless*
            // it matches a known wrap shorthand 0/1/2 AND it looks like wrap was intended.
            // (If you want zero ambiguity, require wrap to be string, or only allow wrap constants via string.)
            t->cfg.textAlignment = clay_parse_text_alignment(L, i, t->cfg.textAlignment);
            continue;
        }

        if (clay_is_wrap_token(L, i)) {
            t->cfg.wrapMode = clay_parse_wrap_mode(L, i, t->cfg.wrapMode);
            continue;
        }

        // If it's a string but not clearly wrap/alignment, try wrap first then alignment.
        if (lua_isstring(L, i)) {
            Clay_TextElementConfigWrapMode w = clay_parse_wrap_mode(L, i, (Clay_TextElementConfigWrapMode)-1);
            if ((int)w != -1) { t->cfg.wrapMode = w; continue; }

            t->cfg.textAlignment = clay_parse_text_alignment(L, i, t->cfg.textAlignment);
        }
    }

    return 1;
}

static int l_Text_userData(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    clay_set_ptr_from_lua(L, 2, &t->cfg.userData);
    lua_settop(L, 1);
    return 1;
}

static int l_Text_textColor(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");

    if (lua_isnil(L,2)) { lua_settop(L,1); return 1; }

    // Support packed hex / table / rgba numbers for textColor too.
    Clay_Color c = t->cfg.textColor;
    // For textColor, treat single number as packed (works when only one arg after self).
    int top = lua_gettop(L);
    if (lua_isnumber(L,2) && top == 2) {
        uint32_t hex = (uint32_t)lua_tointeger(L,2);
        c.r = (float)((hex >> 24) & 0xFF);
        c.g = (float)((hex >> 16) & 0xFF);
        c.b = (float)((hex >> 8) & 0xFF);
        c.a = (float)(hex & 0xFF);
        t->cfg.textColor = c;
        lua_settop(L,1);
        return 1;
    }

    // Otherwise delegate
    if (clay_read_color(L, 2, &c)) {
        t->cfg.textColor = c;
    }
    lua_settop(L, 1);
    return 1;
}

static int l_Text_fontId(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    t->cfg.fontId = (uint16_t)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Text_fontSize(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    t->cfg.fontSize = (uint16_t)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Text_letterSpacing(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    t->cfg.letterSpacing = (uint16_t)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Text_lineHeight(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    t->cfg.lineHeight = (uint16_t)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Text_wrapMode(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    t->cfg.wrapMode = (Clay_TextElementConfigWrapMode)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Text_textAlignment(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return luaL_error(L, "text builder is not active (already done?)");
    t->cfg.textAlignment = (Clay_TextAlignment)luaL_checkinteger(L, 2);
    lua_settop(L, 1);
    return 1;
}

static int l_Text_close(lua_State *L) {
    LuaClayTextBuilder *t = check_text_builder(L, 1);
    if (!t->active) return 0;
    text_builder_emit(L, t);
    return 0;
}

static int l_Text_done(lua_State *L) { return l_Text_close(L); }

static int l_Text_gc(lua_State *L) {
    LuaClayTextBuilder *t = (LuaClayTextBuilder*)luaL_testudata(L, 1, "ClayTextBuilder");
    if (!t) return 0;
    if (t->active) {
        clay_unref_tagged(L, &t->cfg.userData);
        t->active = 0;
    }
    return 0;
}

static void Clay_CreateElementBuilderMetatable(lua_State *L) {
    if (luaL_newmetatable(L, "ClayElementBuilder")) {
        // Fluent Builder API
        lua_pushcfunction(L, l_Elem_id); lua_setfield(L, -2, "id");
        lua_pushcfunction(L, l_Elem_alias); lua_setfield(L, -2, "alias");
        lua_pushcfunction(L, l_Elem_layoutDirection); lua_setfield(L, -2, "layoutDirection");
        lua_pushcfunction(L, l_Elem_childGap); lua_setfield(L, -2, "childGap");
        lua_pushcfunction(L, l_Elem_childAlignment); lua_setfield(L, -2, "childAlignment");
        lua_pushcfunction(L, l_Elem_padding); lua_setfield(L, -2, "padding");
        lua_pushcfunction(L, l_Elem_width); lua_setfield(L, -2, "width");
        lua_pushcfunction(L, l_Elem_height); lua_setfield(L, -2, "height");
        lua_pushcfunction(L, l_Elem_backgroundColor); lua_setfield(L, -2, "backgroundColor");
        lua_pushcfunction(L, l_Elem_transition); lua_setfield(L, -2, "transition");
        lua_pushcfunction(L, l_Elem_cornerRadius); lua_setfield(L, -2, "cornerRadius");
        lua_pushcfunction(L, l_Elem_borderColor); lua_setfield(L, -2, "borderColor");
        lua_pushcfunction(L, l_Elem_borderWidth); lua_setfield(L, -2, "borderWidth");

        lua_pushcfunction(L, l_Elem_clip); lua_setfield(L, -2, "clip");
        lua_pushcfunction(L, l_Elem_clipHorizontal); lua_setfield(L, -2, "clipHorizontal");
        lua_pushcfunction(L, l_Elem_clipVertical); lua_setfield(L, -2, "clipVertical");
        lua_pushcfunction(L, l_Elem_childOffset); lua_setfield(L, -2, "childOffset");
        lua_pushcfunction(L, l_Elem_aspectRatio); lua_setfield(L, -2, "aspectRatio");
        lua_pushcfunction(L, l_Elem_imageData); lua_setfield(L, -2, "imageData");
        lua_pushcfunction(L, l_Elem_customData); lua_setfield(L, -2, "customData");
        lua_pushcfunction(L, l_Elem_userData); lua_setfield(L, -2, "userData");

        lua_pushcfunction(L, l_Elem_style); lua_setfield(L, -2, "style");
        lua_pushcfunction(L, l_Elem_open);  lua_setfield(L, -2, "open");
        lua_pushcfunction(L, l_Elem_close); lua_setfield(L, -2, "close");

        // ---- SUGAR for Builder API ----
        lua_pushcfunction(L, l_Elem_bg); lua_setfield(L, -2, "bg");
        lua_pushcfunction(L, l_Elem_border); lua_setfield(L, -2, "border");

        lua_pushcfunction(L, l_Elem_scrollable); lua_setfield(L, -2, "scrollable"); //sugar for 'clip'

        lua_pushcfunction(L, l_Elem_row); lua_setfield(L, -2, "row");
        lua_pushcfunction(L, l_Elem_col); lua_setfield(L, -2, "col");
        lua_pushcfunction(L, l_Elem_gap); lua_setfield(L, -2, "gap");
        lua_pushcfunction(L, l_Elem_align); lua_setfield(L, -2, "align");

        lua_pushcfunction(L, l_Elem_alignLT); lua_setfield(L, -2, "alignLT");
        lua_pushcfunction(L, l_Elem_alignCT); lua_setfield(L, -2, "alignCT");
        lua_pushcfunction(L, l_Elem_alignRT); lua_setfield(L, -2, "alignRT");
        lua_pushcfunction(L, l_Elem_alignLC); lua_setfield(L, -2, "alignLC");
        lua_pushcfunction(L, l_Elem_alignCC); lua_setfield(L, -2, "alignCC");
        lua_pushcfunction(L, l_Elem_alignRC); lua_setfield(L, -2, "alignRC");
        lua_pushcfunction(L, l_Elem_alignLB); lua_setfield(L, -2, "alignLB");
        lua_pushcfunction(L, l_Elem_alignCB); lua_setfield(L, -2, "alignCB");
        lua_pushcfunction(L, l_Elem_alignRB); lua_setfield(L, -2, "alignRB");

        lua_pushcfunction(L, l_Elem_w); lua_setfield(L, -2, "w");
        lua_pushcfunction(L, l_Elem_h); lua_setfield(L, -2, "h");
        lua_pushcfunction(L, l_Elem_size); lua_setfield(L, -2, "size");
        lua_pushcfunction(L, l_Elem_growW); lua_setfield(L, -2, "wGrow");
        lua_pushcfunction(L, l_Elem_growH); lua_setfield(L, -2, "hGrow");
        lua_pushcfunction(L, l_Elem_growSize); lua_setfield(L, -2, "growSize");
        lua_pushcfunction(L, l_Elem_fitW); lua_setfield(L, -2, "wFit");
        lua_pushcfunction(L, l_Elem_fitH); lua_setfield(L, -2, "hFit");
        lua_pushcfunction(L, l_Elem_fitSize); lua_setfield(L, -2, "fitSize");
        lua_pushcfunction(L, l_Elem_wPct); lua_setfield(L, -2, "wPct");
        lua_pushcfunction(L, l_Elem_hPct); lua_setfield(L, -2, "hPct");
        lua_pushcfunction(L, l_Elem_percentSize); lua_setfield(L, -2, "percentSize");

        // Floating configuration is only available through the scoped builder.
        lua_pushcfunction(L, l_Elem_floating);       lua_setfield(L, -2, "floating");

        // -- SUGAR for TEXT element
        lua_pushcfunction(L, l_Text_wrap);  lua_setfield(L, -2, "wrap");
        lua_pushcfunction(L, l_Text_align); lua_setfield(L, -2, "align");

        lua_pushcfunction(L, l_Elem_gc); lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

static void Clay_CreateFloatingBuilderMetatable(lua_State *L) {
    if (luaL_newmetatable(L, "ClayFloatingBuilder")) {
        lua_pushcfunction(L, l_Floating_parent); lua_setfield(L, -2, "parent");
        lua_pushcfunction(L, l_Floating_root); lua_setfield(L, -2, "root");
        lua_pushcfunction(L, l_Floating_target); lua_setfield(L, -2, "to");
        lua_pushcfunction(L, l_Floating_attach); lua_setfield(L, -2, "attach");
        lua_pushcfunction(L, l_Floating_offset); lua_setfield(L, -2, "offset");
        lua_pushcfunction(L, l_Floating_expand); lua_setfield(L, -2, "expand");
        lua_pushcfunction(L, l_Floating_zIndex); lua_setfield(L, -2, "zIndex");
        lua_pushcfunction(L, l_Floating_pointerCapture); lua_setfield(L, -2, "pointerCapture");
        lua_pushcfunction(L, l_Floating_pointerCaptureMode); lua_setfield(L, -2, "pointerCaptureMode");
        lua_pushcfunction(L, l_Floating_clipTo); lua_setfield(L, -2, "clipTo");
        lua_pushcfunction(L, l_Floating_clipToParent); lua_setfield(L, -2, "clipToParent");
        lua_pushcfunction(L, l_Floating_done); lua_setfield(L, -2, "done");
        lua_pushcfunction(L, l_Floating_gc); lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

static void Clay_CreateTextBuilderMetatable(lua_State *L) {
    if (luaL_newmetatable(L, "ClayTextBuilder")) {
        lua_pushcfunction(L, l_Text_userData); lua_setfield(L, -2, "userData");
        lua_pushcfunction(L, l_Text_textColor); lua_setfield(L, -2, "textColor");
        lua_pushcfunction(L, l_Text_fontId); lua_setfield(L, -2, "fontId");
        lua_pushcfunction(L, l_Text_fontSize); lua_setfield(L, -2, "fontSize");
        lua_pushcfunction(L, l_Text_letterSpacing); lua_setfield(L, -2, "letterSpacing");
        lua_pushcfunction(L, l_Text_lineHeight); lua_setfield(L, -2, "lineHeight");
        lua_pushcfunction(L, l_Text_wrapMode); lua_setfield(L, -2, "wrapMode");
        lua_pushcfunction(L, l_Text_textAlignment); lua_setfield(L, -2, "textAlignment");
        lua_pushcfunction(L, l_Text_close); lua_setfield(L, -2, "close");
        lua_pushcfunction(L, l_Text_done); lua_setfield(L, -2, "done");
        lua_pushcfunction(L, l_Text_gc); lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

/* ---------------- Core API wrappers ---------------- */
static int l_Clay_GetCurrentContext(lua_State *L) {
    Clay_Context* ctx = Clay_GetCurrentContext();
    if (ctx) lua_pushlightuserdata(L, ctx);
    else lua_pushnil(L);
    return 1;
}

static int l_Clay_MinMemorySize(lua_State *L) {
    uint32_t size = Clay_MinMemorySize();
    lua_pushinteger(L, size);
    return 1;
}

static int l_Clay_CreateArenaWithCapacityAndMemory(lua_State *L) {
    size_t capacity = (size_t)luaL_checkinteger(L, 1);
    void* memory = lua_islightuserdata(L, 2) ? lua_touserdata(L, 2) : NULL;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(capacity, memory);
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)arena.capacity); lua_setfield(L, -2, "capacity");
    lua_pushlightuserdata(L, arena.memory); lua_setfield(L, -2, "memory");
    return 1;
}

static int l_Clay_CloseElement(lua_State *L) {
    clay_close_open_element(L, Clay_GetCurrentContext());
    return 0;
}

static void clay_push_element_id_table(lua_State *L, Clay_ElementId eid, Clay_String explicit_sid)
{
    lua_newtable(L);
    lua_pushinteger(L, eid.id);     lua_setfield(L, -2, "id");
    lua_pushinteger(L, eid.offset); lua_setfield(L, -2, "offset");
    lua_pushinteger(L, eid.baseId); lua_setfield(L, -2, "baseId");

    if (explicit_sid.chars && explicit_sid.length > 0) {
        lua_pushlstring(L, explicit_sid.chars, explicit_sid.length);
        lua_setfield(L, -2, "stringId");
    } else if (eid.stringId.chars && eid.stringId.length > 0) {
        lua_pushlstring(L, eid.stringId.chars, eid.stringId.length);
        lua_setfield(L, -2, "stringId");
    } else {
        lua_pushnil(L); lua_setfield(L, -2, "stringId");
    }
}

static int l_Clay_Id(lua_State *L) {
    Clay_String s = Clay_BorrowLuaString(L, 1);
    uint32_t index = (uint32_t)luaL_optinteger(L, 2, 0);
    bool isLocal = lua_toboolean(L, 3);

    Clay_ElementId eid;
    if (index > 0) {
        eid = Clay__HashStringWithOffset(s, index, isLocal ? Clay__GetParentElementId() : 0);
    } else {
        eid = Clay__HashString(s, isLocal ? Clay__GetParentElementId() : 0);
    }

    clay_push_element_id_table(L, eid, s);
    return 1;
}

static int l_Clay_Keyed(lua_State *L) {
    luaL_checkany(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    Clay_Context *context = Clay_GetCurrentContext();
    if (!context || g_ClayBuildingContext != context) {
        return luaL_error(L, "clay.keyed(key, fn) must run between beginLayout() and endLayoutFast()");
    }
    Clay_LayoutElement *parent = clay_current_open_element(context);
    uint32_t parentId = parent ? parent->id : 0;

    uint32_t keyParentId;
    uint32_t keyBaseId = parent ? parent->id : clay_auto_root_parent_id();
    if (g_ClayKeyScope && g_ClayKeyScope->context == context &&
        g_ClayKeyScope->parentId == parentId) {
        keyBaseId = g_ClayKeyScope->keyParentId;
    }
    if (lua_isnumber(L, 1)) {
        keyParentId = Clay__HashNumber((uint32_t)luaL_checkinteger(L, 1), keyBaseId).id;
    } else if (lua_isstring(L, 1)) {
        keyParentId = Clay__HashString(Clay_BorrowLuaString(L, 1), keyBaseId).id;
    } else {
        return luaL_argerror(L, 1, "key must be a string or integer");
    }

    ClayKeyScope scope = {
        .context = context,
        .parentId = parentId,
        .keyParentId = keyParentId,
        .nextOffset = 0,
        .previous = g_ClayKeyScope,
    };
    g_ClayKeyScope = &scope;

    lua_getglobal(L, "debug");
    lua_getfield(L, -1, "traceback");
    lua_remove(L, -2);
    int tracebackIndex = lua_gettop(L);
    lua_pushvalue(L, 2);
    int status = lua_pcall(L, 0, 0, tracebackIndex);
    g_ClayKeyScope = scope.previous;
    if (status != LUA_OK) {
        const char *error = lua_tostring(L, -1);
        char message[4096];
        snprintf(message, sizeof(message), "%s", error ? error : "(unknown)");
        lua_pop(L, 2);
        return luaL_error(L, "clay.keyed() failed:\n%s", message);
    }
    lua_pop(L, 1);
    return 0;
}

static int l_Clay_BeginLayout(lua_State *L) {
    (void)L;
    Clay_BeginLayout();
    g_ClayBuildingContext = Clay_GetCurrentContext();
    return 0;
}

typedef struct { Clay_RenderCommandArray array; int index; } Clay_IteratorState;

static int clay_iter_next(lua_State *L) {
    Clay_IteratorState* it = (Clay_IteratorState*)lua_touserdata(L, lua_upvalueindex(1));
    if (!it || it->index >= it->array.length) return 0;

    Clay_RenderCommand* cmd = &it->array.internalArray[it->index++];

    Clay_RenderCommand** udata = (Clay_RenderCommand**)lua_newuserdata(L, sizeof(Clay_RenderCommand*));
    *udata = cmd;
    luaL_setmetatable(L, "ClayCommand");
    return 1;
}

static int l_Clay_EndLayoutIter(lua_State *L) {
    Clay_IteratorState* it = (Clay_IteratorState*)lua_newuserdata(L, sizeof(Clay_IteratorState));
    *it = (Clay_IteratorState){0};
    it->array = Clay_EndLayout((float)luaL_optnumber(L, 1, 0.0));
    if (g_ClayBuildingContext == Clay_GetCurrentContext()) g_ClayBuildingContext = NULL;
    it->index = 0;

    lua_pushcclosure(L, clay_iter_next, 1);
    return 1;
}

/* Fast render iterator result slots:
 * type, x, y, width, height, r, g, b, a, topLeft, topRight, bottomLeft,
 * bottomRight, borderLeft, borderRight, borderTop, borderBottom, text,
 * fontId, fontSize, letterSpacing, lineHeight, payload, clipHorizontal,
 * clipVertical.
 *
 * Keeping a fixed result shape lets Lua render a command without allocating a
 * command userdata or crossing the C boundary for each individual property.
 */
enum { CLAY_FAST_COMMAND_RESULT_COUNT = 25 };

static void clay_push_nil_values(lua_State *L, int count) {
    for (int i = 0; i < count; ++i) lua_pushnil(L);
}

static int clay_push_pointer_value(lua_State *L, void **slot) {
    void *p = *slot;
    if (p == NULL) {
        lua_pushnil(L);
    } else if (clay_is_ref_tag(p)) {
        int ref = clay_ref_from_tag(p);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        *slot = NULL;
    } else {
        lua_pushlightuserdata(L, p);
    }
    return 1;
}

static int clay_iter_next_fast(lua_State *L) {
    Clay_IteratorState* it = (Clay_IteratorState*)lua_touserdata(L, lua_upvalueindex(1));
    if (!it || it->index >= it->array.length) return 0;
    if (!lua_checkstack(L, CLAY_FAST_COMMAND_RESULT_COUNT))
        return luaL_error(L, "not enough Lua stack space for Clay render command");

    Clay_RenderCommand* cmd = &it->array.internalArray[it->index++];
    lua_pushinteger(L, cmd->commandType);

    lua_pushnumber(L, cmd->boundingBox.x);
    lua_pushnumber(L, cmd->boundingBox.y);
    lua_pushnumber(L, cmd->boundingBox.width);
    lua_pushnumber(L, cmd->boundingBox.height);

    switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
            lua_pushnumber(L, cmd->renderData.rectangle.backgroundColor.r);
            lua_pushnumber(L, cmd->renderData.rectangle.backgroundColor.g);
            lua_pushnumber(L, cmd->renderData.rectangle.backgroundColor.b);
            lua_pushnumber(L, cmd->renderData.rectangle.backgroundColor.a);
            break;
        case CLAY_RENDER_COMMAND_TYPE_BORDER:
            lua_pushnumber(L, cmd->renderData.border.color.r);
            lua_pushnumber(L, cmd->renderData.border.color.g);
            lua_pushnumber(L, cmd->renderData.border.color.b);
            lua_pushnumber(L, cmd->renderData.border.color.a);
            break;
        case CLAY_RENDER_COMMAND_TYPE_TEXT:
            lua_pushnumber(L, cmd->renderData.text.textColor.r);
            lua_pushnumber(L, cmd->renderData.text.textColor.g);
            lua_pushnumber(L, cmd->renderData.text.textColor.b);
            lua_pushnumber(L, cmd->renderData.text.textColor.a);
            break;
        case CLAY_RENDER_COMMAND_TYPE_IMAGE:
            lua_pushnumber(L, cmd->renderData.image.backgroundColor.r);
            lua_pushnumber(L, cmd->renderData.image.backgroundColor.g);
            lua_pushnumber(L, cmd->renderData.image.backgroundColor.b);
            lua_pushnumber(L, cmd->renderData.image.backgroundColor.a);
            break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            lua_pushnumber(L, cmd->renderData.custom.backgroundColor.r);
            lua_pushnumber(L, cmd->renderData.custom.backgroundColor.g);
            lua_pushnumber(L, cmd->renderData.custom.backgroundColor.b);
            lua_pushnumber(L, cmd->renderData.custom.backgroundColor.a);
            break;
        default:
            clay_push_nil_values(L, 4);
            break;
    }

    switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
            lua_pushnumber(L, cmd->renderData.rectangle.cornerRadius.topLeft);
            lua_pushnumber(L, cmd->renderData.rectangle.cornerRadius.topRight);
            lua_pushnumber(L, cmd->renderData.rectangle.cornerRadius.bottomLeft);
            lua_pushnumber(L, cmd->renderData.rectangle.cornerRadius.bottomRight);
            break;
        case CLAY_RENDER_COMMAND_TYPE_BORDER:
            lua_pushnumber(L, cmd->renderData.border.cornerRadius.topLeft);
            lua_pushnumber(L, cmd->renderData.border.cornerRadius.topRight);
            lua_pushnumber(L, cmd->renderData.border.cornerRadius.bottomLeft);
            lua_pushnumber(L, cmd->renderData.border.cornerRadius.bottomRight);
            break;
        case CLAY_RENDER_COMMAND_TYPE_IMAGE:
            lua_pushnumber(L, cmd->renderData.image.cornerRadius.topLeft);
            lua_pushnumber(L, cmd->renderData.image.cornerRadius.topRight);
            lua_pushnumber(L, cmd->renderData.image.cornerRadius.bottomLeft);
            lua_pushnumber(L, cmd->renderData.image.cornerRadius.bottomRight);
            break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            lua_pushnumber(L, cmd->renderData.custom.cornerRadius.topLeft);
            lua_pushnumber(L, cmd->renderData.custom.cornerRadius.topRight);
            lua_pushnumber(L, cmd->renderData.custom.cornerRadius.bottomLeft);
            lua_pushnumber(L, cmd->renderData.custom.cornerRadius.bottomRight);
            break;
        default:
            clay_push_nil_values(L, 4);
            break;
    }

    if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_BORDER) {
        lua_pushnumber(L, cmd->renderData.border.width.left);
        lua_pushnumber(L, cmd->renderData.border.width.right);
        lua_pushnumber(L, cmd->renderData.border.width.top);
        lua_pushnumber(L, cmd->renderData.border.width.bottom);
    } else {
        clay_push_nil_values(L, 4);
    }

    if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_TEXT) {
        lua_pushlstring(L, cmd->renderData.text.stringContents.chars,
                        cmd->renderData.text.stringContents.length);
        lua_pushinteger(L, cmd->renderData.text.fontId);
        lua_pushinteger(L, cmd->renderData.text.fontSize);
        lua_pushinteger(L, cmd->renderData.text.letterSpacing);
        lua_pushinteger(L, cmd->renderData.text.lineHeight);
    } else {
        clay_push_nil_values(L, 5);
    }

    if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE) {
        clay_push_pointer_value(L, &cmd->renderData.image.imageData);
    } else if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_CUSTOM) {
        clay_push_pointer_value(L, &cmd->renderData.custom.customData);
    } else {
        lua_pushnil(L);
    }

    if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_SCISSOR_START ||
        cmd->commandType == CLAY_RENDER_COMMAND_TYPE_SCISSOR_END) {
        lua_pushboolean(L, cmd->renderData.clip.horizontal);
        lua_pushboolean(L, cmd->renderData.clip.vertical);
    } else {
        clay_push_nil_values(L, 2);
    }

    return CLAY_FAST_COMMAND_RESULT_COUNT;
}

static int l_Clay_EndLayoutFast(lua_State *L) {
    Clay_IteratorState* it = (Clay_IteratorState*)lua_newuserdata(L, sizeof(Clay_IteratorState));
    *it = (Clay_IteratorState){0};
    it->array = Clay_EndLayout((float)luaL_optnumber(L, 1, 0.0));
    if (g_ClayBuildingContext == Clay_GetCurrentContext()) g_ClayBuildingContext = NULL;
    it->index = 0;

    lua_pushcclosure(L, clay_iter_next_fast, 1);
    return 1;
}

static Clay_RenderCommand* checkcmd(lua_State *L) {
    return *(Clay_RenderCommand**)luaL_checkudata(L, 1, "ClayCommand");
}

static int l_ClayCmd_Type(lua_State *L) { Clay_RenderCommand* cmd = checkcmd(L); lua_pushinteger(L, cmd->commandType); return 1; }
static int l_ClayCmd_Id(lua_State *L) { Clay_RenderCommand* cmd = checkcmd(L); lua_pushinteger(L, cmd->id); return 1; }
static int l_ClayCmd_zIndex(lua_State *L) { Clay_RenderCommand* cmd = checkcmd(L); lua_pushinteger(L, cmd->zIndex); return 1; }
static int l_ClayCmd_Bounds(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    lua_pushnumber(L, cmd->boundingBox.x);
    lua_pushnumber(L, cmd->boundingBox.y);
    lua_pushnumber(L, cmd->boundingBox.width);
    lua_pushnumber(L, cmd->boundingBox.height);
    return 4;
}

static int l_ClayCmd_Color(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
            lua_pushnumber(L, cmd->renderData.rectangle.backgroundColor.r);
            lua_pushnumber(L, cmd->renderData.rectangle.backgroundColor.g);
            lua_pushnumber(L, cmd->renderData.rectangle.backgroundColor.b);
            lua_pushnumber(L, cmd->renderData.rectangle.backgroundColor.a);
            return 4;
        case CLAY_RENDER_COMMAND_TYPE_TEXT:
            lua_pushnumber(L, cmd->renderData.text.textColor.r);
            lua_pushnumber(L, cmd->renderData.text.textColor.g);
            lua_pushnumber(L, cmd->renderData.text.textColor.b);
            lua_pushnumber(L, cmd->renderData.text.textColor.a);
            return 4;
        case CLAY_RENDER_COMMAND_TYPE_IMAGE:
            lua_pushnumber(L, cmd->renderData.image.backgroundColor.r);
            lua_pushnumber(L, cmd->renderData.image.backgroundColor.g);
            lua_pushnumber(L, cmd->renderData.image.backgroundColor.b);
            lua_pushnumber(L, cmd->renderData.image.backgroundColor.a);
            return 4;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            lua_pushnumber(L, cmd->renderData.custom.backgroundColor.r);
            lua_pushnumber(L, cmd->renderData.custom.backgroundColor.g);
            lua_pushnumber(L, cmd->renderData.custom.backgroundColor.b);
            lua_pushnumber(L, cmd->renderData.custom.backgroundColor.a);
            return 4;
        case CLAY_RENDER_COMMAND_TYPE_BORDER:
            lua_pushnumber(L, cmd->renderData.border.color.r);
            lua_pushnumber(L, cmd->renderData.border.color.g);
            lua_pushnumber(L, cmd->renderData.border.color.b);
            lua_pushnumber(L, cmd->renderData.border.color.a);
            return 4;
        default:
            return 0;
    }
}

static int l_ClayCmd_Text(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    if (cmd->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT) return 0;
    lua_pushlstring(L, cmd->renderData.text.stringContents.chars, cmd->renderData.text.stringContents.length);
    lua_pushinteger(L, cmd->renderData.text.fontId);
    lua_pushinteger(L, cmd->renderData.text.fontSize);
    lua_pushinteger(L, cmd->renderData.text.letterSpacing);
    lua_pushinteger(L, cmd->renderData.text.lineHeight);
    return 5;
}

static int l_ClayCmd_CornerRadius(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_RECTANGLE) {
        lua_pushnumber(L, cmd->renderData.rectangle.cornerRadius.topLeft);
        lua_pushnumber(L, cmd->renderData.rectangle.cornerRadius.topRight);
        lua_pushnumber(L, cmd->renderData.rectangle.cornerRadius.bottomLeft);
        lua_pushnumber(L, cmd->renderData.rectangle.cornerRadius.bottomRight);
        return 4;
    } else if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_BORDER) {
        lua_pushnumber(L, cmd->renderData.border.cornerRadius.topLeft);
        lua_pushnumber(L, cmd->renderData.border.cornerRadius.topRight);
        lua_pushnumber(L, cmd->renderData.border.cornerRadius.bottomLeft);
        lua_pushnumber(L, cmd->renderData.border.cornerRadius.bottomRight);
        return 4;
    } else if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE) {
        lua_pushnumber(L, cmd->renderData.image.cornerRadius.topLeft);
        lua_pushnumber(L, cmd->renderData.image.cornerRadius.topRight);
        lua_pushnumber(L, cmd->renderData.image.cornerRadius.bottomLeft);
        lua_pushnumber(L, cmd->renderData.image.cornerRadius.bottomRight);
        return 4;
    } else if (cmd->commandType == CLAY_RENDER_COMMAND_TYPE_CUSTOM) {
        lua_pushnumber(L, cmd->renderData.custom.cornerRadius.topLeft);
        lua_pushnumber(L, cmd->renderData.custom.cornerRadius.topRight);
        lua_pushnumber(L, cmd->renderData.custom.cornerRadius.bottomLeft);
        lua_pushnumber(L, cmd->renderData.custom.cornerRadius.bottomRight);
        return 4;
    }
    return 0;
}

static int l_ClayCmd_BorderWidth(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    if (cmd->commandType != CLAY_RENDER_COMMAND_TYPE_BORDER) return 0;
    lua_pushnumber(L, cmd->renderData.border.width.left);
    lua_pushnumber(L, cmd->renderData.border.width.right);
    lua_pushnumber(L, cmd->renderData.border.width.top);
    lua_pushnumber(L, cmd->renderData.border.width.bottom);
    return 4;
}

static int l_ClayCmd_ImageData(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    if (cmd->commandType != CLAY_RENDER_COMMAND_TYPE_IMAGE) return 0;
    void *p = cmd->renderData.image.imageData;
    if (p == NULL) { lua_pushnil(L); return 1; }
    if (clay_is_ref_tag(p)) {
        int ref = clay_ref_from_tag(p);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        cmd->renderData.image.imageData = NULL;
        return 1;
    }
    lua_pushlightuserdata(L, p);
    return 1;
}

static int l_ClayCmd_CustomData(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    if (cmd->commandType != CLAY_RENDER_COMMAND_TYPE_CUSTOM) return 0;
    void *p = cmd->renderData.custom.customData;
    if (p == NULL) { lua_pushnil(L); return 1; }
    if (clay_is_ref_tag(p)) {
        int ref = clay_ref_from_tag(p);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        cmd->renderData.custom.customData = NULL;
        return 1;
    }
    lua_pushlightuserdata(L, p);
    return 1;
}

static int l_ClayCmd_UserData(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    void *p = cmd->userData;
    if (p == NULL) { lua_pushnil(L); return 1; }
    if (clay_is_ref_tag(p)) {
        int ref = clay_ref_from_tag(p);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        cmd->userData = NULL;
        return 1;
    }
    lua_pushlightuserdata(L, p);
    return 1;
}

static int l_ClayCmd_Clip(lua_State *L) {
    Clay_RenderCommand* cmd = checkcmd(L);
    if (cmd->commandType != CLAY_RENDER_COMMAND_TYPE_SCISSOR_START &&
        cmd->commandType != CLAY_RENDER_COMMAND_TYPE_SCISSOR_END)
        return 0;
    lua_pushboolean(L, cmd->renderData.clip.horizontal);
    lua_pushboolean(L, cmd->renderData.clip.vertical);
    return 2;
}

static void Clay_CreateCommandMetatable(lua_State *L) {
    if (luaL_newmetatable(L, "ClayCommand")) {
        lua_pushcfunction(L, l_ClayCmd_Type); lua_setfield(L, -2, "type");
        lua_pushcfunction(L, l_ClayCmd_Id); lua_setfield(L, -2, "id");
        lua_pushcfunction(L, l_ClayCmd_Bounds); lua_setfield(L, -2, "bounds");
        lua_pushcfunction(L, l_ClayCmd_Color); lua_setfield(L, -2, "color");
        lua_pushcfunction(L, l_ClayCmd_Text); lua_setfield(L, -2, "text");
        lua_pushcfunction(L, l_ClayCmd_CornerRadius); lua_setfield(L, -2, "cornerRadius");
        lua_pushcfunction(L, l_ClayCmd_BorderWidth); lua_setfield(L, -2, "borderWidth");
        lua_pushcfunction(L, l_ClayCmd_ImageData); lua_setfield(L, -2, "imageData");
        lua_pushcfunction(L, l_ClayCmd_CustomData); lua_setfield(L, -2, "customData");
        lua_pushcfunction(L, l_ClayCmd_UserData); lua_setfield(L, -2, "userData");
        lua_pushcfunction(L, l_ClayCmd_Clip); lua_setfield(L, -2, "clip");
        lua_pushcfunction(L, l_ClayCmd_zIndex); lua_setfield(L, -2, "zIndex");

        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

static int l_Clay_SetLayoutDimensions(lua_State *L) {
    double w = luaL_checknumber(L, 1);
    double h = luaL_checknumber(L, 2);
    Clay_SetLayoutDimensions((Clay_Dimensions){ (float)w, (float)h });
    return 0;
}

static int l_Clay_SetCurrentContext(lua_State *L) {
    Clay_Context *context = lua_isnil(L, 1) ? NULL : (Clay_Context *)lua_touserdata(L, 1);
    if (!lua_isnil(L, 1) && !lua_islightuserdata(L, 1)) {
        return luaL_argerror(L, 1, "expected a Clay context returned by initialize/createContext");
    }
    Clay_SetCurrentContext(context);
    return 0;
}

static int l_Clay_SetPointerState(lua_State *L) {
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    bool down = lua_toboolean(L, 3);
    Clay_SetPointerState((Clay_Vector2){ (float)x, (float)y }, down);
    return 0;
}

static int l_Clay_GetPointerOverIds(lua_State *L) {
    Clay_ElementIdArray ids = Clay_GetPointerOverIds();
    int targetIndex;
    int oldLength = 0;
    if (lua_istable(L, 1)) {
        targetIndex = lua_absindex(L, 1);
        lua_getfield(L, targetIndex, "n");
        oldLength = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    } else {
        lua_createtable(L, ids.length, 1);
        targetIndex = lua_absindex(L, -1);
    }
    for (int32_t i = 0; i < ids.length; ++i) {
        lua_pushinteger(L, Clay_ElementIdArray_Get(&ids, i)->id);
        lua_rawseti(L, targetIndex, i + 1);
    }
    for (int32_t i = ids.length; i < oldLength; ++i) {
        lua_pushnil(L);
        lua_rawseti(L, targetIndex, i + 1);
    }
    lua_pushinteger(L, ids.length);
    lua_setfield(L, targetIndex, "n");
    if (targetIndex != lua_gettop(L)) lua_pushvalue(L, targetIndex);
    return 1;
}

static int l_Clay_ContextHasActiveMotion(lua_State *L) {
    Clay_Context *context = Clay_GetCurrentContext();
    bool active = false;
    if (context) {
        for (int32_t i = 0; i < context->scrollContainerDatas.length; ++i) {
            Clay__ScrollContainerDataInternal *scroll = Clay__ScrollContainerDataInternalArray_Get(&context->scrollContainerDatas, i);
            if (scroll->pointerScrollActive || scroll->scrollMomentum.x > 0.1f || scroll->scrollMomentum.x < -0.1f ||
                scroll->scrollMomentum.y > 0.1f || scroll->scrollMomentum.y < -0.1f) {
                active = true;
                break;
            }
        }
        for (int32_t i = 0; !active && i < context->transitionDatas.length; ++i) {
            Clay__TransitionDataInternal *transition = Clay__TransitionDataInternalArray_Get(&context->transitionDatas, i);
            active = transition->state != CLAY_TRANSITION_STATE_IDLE;
        }
    }
    lua_pushboolean(L, active);
    return 1;
}

static int l_Clay_UpdateScrollContainers(lua_State *L) {
    bool enable = lua_toboolean(L, 1);
    double dx = luaL_checknumber(L, 2);
    double dy = luaL_checknumber(L, 3);
    double dt = luaL_checknumber(L, 4);
    Clay_UpdateScrollContainers(enable, (Clay_Vector2){(float)dx, (float)dy}, (float)dt);
    return 0;
}

static int l_Clay_GetScrollOffset(lua_State *L) {
    Clay_Vector2 off = Clay_GetScrollOffset();
    lua_newtable(L);
    lua_pushnumber(L, off.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, off.y); lua_setfield(L, -2, "y");
    return 1;
}

static int l_Clay_GetElementData(lua_State *L) {
    Clay_ElementId elid = clay_check_element_id(L, 1);

    Clay_ElementData d = Clay_GetElementData(elid);
    lua_newtable(L);
    lua_pushnumber(L, d.boundingBox.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, d.boundingBox.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, d.boundingBox.width); lua_setfield(L, -2, "width");
    lua_pushnumber(L, d.boundingBox.height); lua_setfield(L, -2, "height");
    lua_pushboolean(L, d.found); lua_setfield(L, -2, "found");
    return 1;
}

static void ClayErrorPrinter(Clay_ErrorData err) {
    fprintf(stderr, "[Clay Error] %.*s\n", (int)err.errorText.length, err.errorText.chars);
}

static int l_Clay_Initialize(lua_State *L) {
    size_t capacity = (size_t)luaL_checkinteger(L, 1);
    float width  = (float)luaL_checknumber(L, 2);
    float height = (float)luaL_checknumber(L, 3);
    Clay_Context *previous = Clay_GetCurrentContext();

    void *mem = malloc(capacity);
    if (!mem)
        return luaL_error(L, "malloc failed");

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(capacity, mem);
    Clay_Context *ctx = Clay_Initialize(
        arena,
        (Clay_Dimensions){ width, height },
        (Clay_ErrorHandler){ ClayErrorPrinter, NULL }
    );

    if (!ctx) {
        free(mem);
        Clay_SetCurrentContext(previous);
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }

    if (!clay_register_owned_context(mem, capacity, ctx)) {
        Clay_SetCurrentContext(previous);
        free(mem);
        return luaL_error(L, "failed to track Clay context arena");
    }

    Clay_SetMeasureTextFunction(Bridge_MeasureTextFunction, NULL);

    lua_pushlightuserdata(L, mem);
    lua_pushlightuserdata(L, ctx);
    return 2;
}

static int l_Clay_CreateContext(lua_State *L) {
    size_t capacity = (size_t)luaL_checkinteger(L, 1);
    float width = (float)luaL_checknumber(L, 2);
    float height = (float)luaL_checknumber(L, 3);
    Clay_Context *previous = Clay_GetCurrentContext();
    void *memory = malloc(capacity);
    if (!memory) return luaL_error(L, "malloc failed while creating retained Clay context");

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(capacity, memory);
    Clay_Context *context = Clay_Initialize(
        arena,
        (Clay_Dimensions){ width, height },
        (Clay_ErrorHandler){ ClayErrorPrinter, NULL }
    );
    if (!context) {
        free(memory);
        Clay_SetCurrentContext(previous);
        lua_pushnil(L);
        return 1;
    }
    if (!clay_register_owned_context(memory, capacity, context)) {
        Clay_SetCurrentContext(previous);
        free(memory);
        return luaL_error(L, "failed to track retained Clay context arena");
    }

    Clay_SetMeasureTextFunction(Bridge_MeasureTextFunction, NULL);
    Clay_SetCurrentContext(previous);
    lua_pushlightuserdata(L, context);
    return 1;
}

static int l_Clay_DestroyContext(lua_State *L) {
    Clay_Context *context = (Clay_Context *)lua_touserdata(L, 1);
    if (!context) return 0;
    ClayOwnedContext *previous = NULL;
    ClayOwnedContext *owned = clay_find_owned_context(context, &previous);
    if (!owned) return luaL_argerror(L, 1, "context is not owned by this Clay binding");

    Clay_Context *current = Clay_GetCurrentContext();
    if (g_ClayBuildingContext == context) g_ClayBuildingContext = NULL;
    Clay_Context *fallback = previous ? previous->context : owned->next ? owned->next->context : NULL;
    if (current == context) Clay_SetCurrentContext(fallback);
    if (previous) previous->next = owned->next;
    else g_ClayOwnedContexts = owned->next;
    free(owned->memory);
    free(owned);
    return 0;
}

static int l_Clay_SetAliasRegistrar(lua_State *L) {
    if (!lua_isnil(L, 1) && !lua_isfunction(L, 1)) {
        return luaL_argerror(L, 1, "expected function or nil");
    }
    if (g_ClayAliasRegistrarRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_ClayAliasRegistrarRef);
        g_ClayAliasRegistrarRef = LUA_NOREF;
    }
    if (lua_isfunction(L, 1)) {
        lua_pushvalue(L, 1);
        g_ClayAliasRegistrarRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int l_Clay_Shutdown(lua_State* L) {
    if (g_ClayAliasRegistrarRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_ClayAliasRegistrarRef);
        g_ClayAliasRegistrarRef = LUA_NOREF;
    }
    while (g_ClayOwnedContexts) {
        ClayOwnedContext *owned = g_ClayOwnedContexts;
        g_ClayOwnedContexts = owned->next;
        free(owned->memory);
        free(owned);
    }
    g_ClayKeyScope = NULL;
    g_ClayBuildingContext = NULL;
    Clay_SetCurrentContext(NULL);
    return 0;
}

static int l_Clay_Hovered(lua_State *L) { lua_pushboolean(L, Clay_Hovered()); return 1; }
static int l_Clay_PointerOver(lua_State *L) { Clay_ElementId elid = clay_check_element_id(L, 1); lua_pushboolean(L, Clay_PointerOver(elid)); return 1; }

// --- Scroll container data (as in your original snippet) ---
static int l_Clay_GetScrollContainerData(lua_State *L) {
    Clay_ElementId elid = clay_check_element_id(L, 1);

    Clay_ScrollContainerData data = Clay_GetScrollContainerData(elid);

    if (!data.found) { lua_pushnil(L); return 1; }

    lua_newtable(L);

    lua_newtable(L);
    lua_pushnumber(L, data.scrollPosition ? data.scrollPosition->x : 0.0); lua_setfield(L, -2, "x");
    lua_pushnumber(L, data.scrollPosition ? data.scrollPosition->y : 0.0); lua_setfield(L, -2, "y");
    lua_setfield(L, -2, "scrollPosition");

    lua_newtable(L);
    lua_pushnumber(L, data.scrollContainerDimensions.width); lua_setfield(L, -2, "width");
    lua_pushnumber(L, data.scrollContainerDimensions.height); lua_setfield(L, -2, "height");
    lua_setfield(L, -2, "scrollContainerDimensions");

    lua_newtable(L);
    lua_pushnumber(L, data.contentDimensions.width); lua_setfield(L, -2, "width");
    lua_pushnumber(L, data.contentDimensions.height); lua_setfield(L, -2, "height");
    lua_setfield(L, -2, "contentDimensions");

    lua_newtable(L);
    lua_pushboolean(L, data.config.horizontal); lua_setfield(L, -2, "horizontal");
    lua_pushboolean(L, data.config.vertical); lua_setfield(L, -2, "vertical");

    lua_newtable(L);
    lua_pushnumber(L, data.config.childOffset.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, data.config.childOffset.y); lua_setfield(L, -2, "y");
    lua_setfield(L, -2, "childOffset");
    lua_setfield(L, -2, "config");

    lua_pushboolean(L, data.found); lua_setfield(L, -2, "found");
    return 1;
}

static int l_Clay_SetScrollContainerPosition(lua_State *L) {
    Clay_ElementId elid = clay_check_element_id(L, 1);

    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);

    Clay_ScrollContainerData data = Clay_GetScrollContainerData(elid);
    if (!data.found || data.scrollPosition == NULL) return 0;

    data.scrollPosition->x = x;
    data.scrollPosition->y = y;
    return 0;
}

static int l_Clay_SetScrollOffset(lua_State *L) {
    Clay_ElementId elid = clay_check_element_id(L, 1);

    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);

    Clay_Context* ctx = Clay_GetCurrentContext();
    for (int32_t i = 0; i < ctx->scrollContainerDatas.length; ++i) {
        Clay__ScrollContainerDataInternal *mapping = Clay__ScrollContainerDataInternalArray_Get(&ctx->scrollContainerDatas, i);
        if (mapping->elementId == elid.id) {
            mapping->scrollPosition.x = x;
            mapping->scrollPosition.y = y;
            mapping->openThisFrame = true;
            break;
        }
    }
    return 0;
}

static int l_Clay_SetDebugModeEnabled(lua_State *L) { Clay_SetDebugModeEnabled(lua_toboolean(L, 1)); return 0; }
static int l_Clay_IsDebugModeEnabled(lua_State *L) { lua_pushboolean(L, Clay_IsDebugModeEnabled()); return 1; }
static int l_Clay_SetCullingEnabled(lua_State *L) { Clay_SetCullingEnabled(lua_toboolean(L, 1)); return 0; }
static int l_Clay_GetMaxElementCount(lua_State *L) { lua_pushinteger(L, Clay_GetMaxElementCount()); return 1; }
static int l_Clay_SetMaxElementCount(lua_State *L) { Clay_SetMaxElementCount((int32_t)luaL_checkinteger(L, 1)); return 0; }
static int l_Clay_SetExternalScrollHandlingEnabled(lua_State *L) { Clay_SetExternalScrollHandlingEnabled(lua_toboolean(L, 1)); return 0; }
static int l_Clay_GetMaxMeasureTextCacheWordCount(lua_State *L) { lua_pushinteger(L, Clay_GetMaxMeasureTextCacheWordCount()); return 1; }
static int l_Clay_SetMaxMeasureTextCacheWordCount(lua_State *L) { Clay_SetMaxMeasureTextCacheWordCount((int32_t)luaL_checkinteger(L, 1)); return 0; }
static int l_Clay_ResetMeasureTextCache(lua_State *L) { (void)L; Clay_ResetMeasureTextCache(); return 0; }

int luaopen_clay(lua_State *L) {
    lua_newtable(L);

    lua_pushcfunction(L, l_Clay_BeginLayout); lua_setfield(L, -2, "beginLayout");
    lua_pushcfunction(L, l_Clay_EndLayoutIter); lua_setfield(L, -2, "endLayoutIter");
    lua_pushcfunction(L, l_Clay_EndLayoutFast); lua_setfield(L, -2, "endLayoutFast");

    lua_pushcfunction(L, l_Clay_ElementBuilder_New); lua_setfield(L, -2, "element");
    lua_pushcfunction(L, l_Clay_TextBuilder_New); lua_setfield(L, -2, "text");
    lua_pushcfunction(L, l_Clay_Id); lua_setfield(L, -2, "id");
    lua_pushcfunction(L, l_Clay_Keyed); lua_setfield(L, -2, "keyed");
    lua_pushcfunction(L, l_Clay_CloseElement); lua_setfield(L, -2, "close");

    lua_pushcfunction(L, l_Clay_SetLayoutDimensions); lua_setfield(L, -2, "setLayoutDimensions");
    lua_pushcfunction(L, l_Clay_SetPointerState); lua_setfield(L, -2, "setPointerState");
    lua_pushcfunction(L, l_Clay_GetPointerOverIds); lua_setfield(L, -2, "getPointerOverIds");
    lua_pushcfunction(L, l_Clay_UpdateScrollContainers); lua_setfield(L, -2, "updateScrollContainers");
    lua_pushcfunction(L, l_Clay_GetScrollOffset); lua_setfield(L, -2, "getScrollOffset");
    lua_pushcfunction(L, l_Clay_GetElementData); lua_setfield(L, -2, "getElementData");

    lua_pushcfunction(L, l_Clay_GetCurrentContext); lua_setfield(L, -2, "getCurrentContext");
    lua_pushcfunction(L, l_Clay_SetCurrentContext); lua_setfield(L, -2, "setCurrentContext");
    lua_pushcfunction(L, l_Clay_Initialize); lua_setfield(L, -2, "initialize");
    lua_pushcfunction(L, l_Clay_CreateContext); lua_setfield(L, -2, "createContext");
    lua_pushcfunction(L, l_Clay_DestroyContext); lua_setfield(L, -2, "destroyContext");
    lua_pushcfunction(L, l_Clay_ContextHasActiveMotion); lua_setfield(L, -2, "contextHasActiveMotion");
    lua_pushcfunction(L, l_Clay_SetAliasRegistrar); lua_setfield(L, -2, "_setAliasRegistrar");
    lua_pushcfunction(L, l_Clay_Shutdown); lua_setfield(L, -2, "shutdown");
    lua_pushcfunction(L, l_Clay_MinMemorySize); lua_setfield(L, -2, "minMemorySize");
    lua_pushcfunction(L, l_Clay_CreateArenaWithCapacityAndMemory); lua_setfield(L, -2, "createArenaWithCapacityAndMemory");
    lua_pushcfunction(L, l_Clay_Hovered); lua_setfield(L, -2, "hovered");
    lua_pushcfunction(L, l_Clay_PointerOver); lua_setfield(L, -2, "pointerOver");
    lua_pushcfunction(L, l_Clay_GetScrollContainerData); lua_setfield(L, -2, "getScrollContainerData");
    lua_pushcfunction(L, l_Clay_SetScrollContainerPosition); lua_setfield(L, -2, "setScrollContainerPosition");
    lua_pushcfunction(L, l_Clay_SetScrollOffset); lua_setfield(L, -2, "setScrollOffset");
    lua_pushcfunction(L, l_Clay_SetDebugModeEnabled); lua_setfield(L, -2, "setDebugModeEnabled");
    lua_pushcfunction(L, l_Clay_IsDebugModeEnabled); lua_setfield(L, -2, "isDebugModeEnabled");
    lua_pushcfunction(L, l_Clay_SetCullingEnabled); lua_setfield(L, -2, "setCullingEnabled");
    lua_pushcfunction(L, l_Clay_GetMaxElementCount); lua_setfield(L, -2, "getMaxElementCount");
    lua_pushcfunction(L, l_Clay_SetMaxElementCount); lua_setfield(L, -2, "setMaxElementCount");
    lua_pushcfunction(L, l_Clay_SetExternalScrollHandlingEnabled); lua_setfield(L, -2, "setExternalScrollHandlingEnabled");
    lua_pushcfunction(L, l_Clay_GetMaxMeasureTextCacheWordCount); lua_setfield(L, -2, "getMaxMeasureTextCacheWordCount");
    lua_pushcfunction(L, l_Clay_SetMaxMeasureTextCacheWordCount); lua_setfield(L, -2, "setMaxMeasureTextCacheWordCount");
    lua_pushcfunction(L, l_Clay_ResetMeasureTextCache); lua_setfield(L, -2, "resetMeasureTextCache");

    lua_pushcfunction(L, l_Clay_SetMeasureTextFunction); lua_setfield(L, -2, "setMeasureTextFunction");

    // Export constants (as in your original)
    lua_pushinteger(L, CLAY_RENDER_COMMAND_TYPE_NONE); lua_setfield(L, -2, "RENDER_NONE");
    lua_pushinteger(L, CLAY_RENDER_COMMAND_TYPE_RECTANGLE); lua_setfield(L, -2, "RENDER_RECTANGLE");
    lua_pushinteger(L, CLAY_RENDER_COMMAND_TYPE_BORDER); lua_setfield(L, -2, "RENDER_BORDER");
    lua_pushinteger(L, CLAY_RENDER_COMMAND_TYPE_TEXT); lua_setfield(L, -2, "RENDER_TEXT");
    lua_pushinteger(L, CLAY_RENDER_COMMAND_TYPE_IMAGE); lua_setfield(L, -2, "RENDER_IMAGE");
    lua_pushinteger(L, CLAY_RENDER_COMMAND_TYPE_SCISSOR_START); lua_setfield(L, -2, "RENDER_SCISSOR_START");
    lua_pushinteger(L, CLAY_RENDER_COMMAND_TYPE_SCISSOR_END); lua_setfield(L, -2, "RENDER_SCISSOR_END");
    lua_pushinteger(L, CLAY_RENDER_COMMAND_TYPE_CUSTOM); lua_setfield(L, -2, "RENDER_CUSTOM");

    lua_pushinteger(L, CLAY__SIZING_TYPE_FIT); lua_setfield(L, -2, "SIZING_FIT");
    lua_pushinteger(L, CLAY__SIZING_TYPE_GROW); lua_setfield(L, -2, "SIZING_GROW");
    lua_pushinteger(L, CLAY__SIZING_TYPE_FIXED); lua_setfield(L, -2, "SIZING_FIXED");
    lua_pushinteger(L, CLAY__SIZING_TYPE_PERCENT); lua_setfield(L, -2, "SIZING_PERCENT");

    lua_pushinteger(L, CLAY_ALIGN_X_LEFT); lua_setfield(L, -2, "ALIGN_X_LEFT");
    lua_pushinteger(L, CLAY_ALIGN_X_CENTER); lua_setfield(L, -2, "ALIGN_X_CENTER");
    lua_pushinteger(L, CLAY_ALIGN_X_RIGHT); lua_setfield(L, -2, "ALIGN_X_RIGHT");
    lua_pushinteger(L, CLAY_ALIGN_Y_TOP); lua_setfield(L, -2, "ALIGN_Y_TOP");
    lua_pushinteger(L, CLAY_ALIGN_Y_CENTER); lua_setfield(L, -2, "ALIGN_Y_CENTER");
    lua_pushinteger(L, CLAY_ALIGN_Y_BOTTOM); lua_setfield(L, -2, "ALIGN_Y_BOTTOM");

    lua_pushinteger(L, CLAY_TEXT_ALIGN_LEFT); lua_setfield(L, -2, "TEXT_ALIGN_LEFT");
    lua_pushinteger(L, CLAY_TEXT_ALIGN_CENTER); lua_setfield(L, -2, "TEXT_ALIGN_CENTER");
    lua_pushinteger(L, CLAY_TEXT_ALIGN_RIGHT); lua_setfield(L, -2, "TEXT_ALIGN_RIGHT");

    lua_pushinteger(L, CLAY_TEXT_WRAP_NONE); lua_setfield(L, -2, "TEXT_WRAP_NONE");
    lua_pushinteger(L, CLAY_TEXT_WRAP_WORDS); lua_setfield(L, -2, "TEXT_WRAP_WORDS");
    lua_pushinteger(L, CLAY_TEXT_WRAP_NEWLINES); lua_setfield(L, -2, "TEXT_WRAP_NEWLINES");

    lua_pushinteger(L, CLAY_LEFT_TO_RIGHT); lua_setfield(L, -2, "LEFT_TO_RIGHT");
    lua_pushinteger(L, CLAY_TOP_TO_BOTTOM); lua_setfield(L, -2, "TOP_TO_BOTTOM");

    lua_pushinteger(L, CLAY_ATTACH_POINT_LEFT_TOP);       lua_setfield(L, -2, "ATTACH_POINT_LEFT_TOP");
    lua_pushinteger(L, CLAY_ATTACH_POINT_LEFT_CENTER);    lua_setfield(L, -2, "ATTACH_POINT_LEFT_CENTER");
    lua_pushinteger(L, CLAY_ATTACH_POINT_LEFT_BOTTOM);    lua_setfield(L, -2, "ATTACH_POINT_LEFT_BOTTOM");
    lua_pushinteger(L, CLAY_ATTACH_POINT_CENTER_TOP);     lua_setfield(L, -2, "ATTACH_POINT_CENTER_TOP");
    lua_pushinteger(L, CLAY_ATTACH_POINT_CENTER_CENTER);  lua_setfield(L, -2, "ATTACH_POINT_CENTER_CENTER");
    lua_pushinteger(L, CLAY_ATTACH_POINT_CENTER_BOTTOM);  lua_setfield(L, -2, "ATTACH_POINT_CENTER_BOTTOM");
    lua_pushinteger(L, CLAY_ATTACH_POINT_RIGHT_TOP);      lua_setfield(L, -2, "ATTACH_POINT_RIGHT_TOP");
    lua_pushinteger(L, CLAY_ATTACH_POINT_RIGHT_CENTER);   lua_setfield(L, -2, "ATTACH_POINT_RIGHT_CENTER");
    lua_pushinteger(L, CLAY_ATTACH_POINT_RIGHT_BOTTOM);   lua_setfield(L, -2, "ATTACH_POINT_RIGHT_BOTTOM");

    lua_pushinteger(L, CLAY_ATTACH_TO_NONE);               lua_setfield(L, -2, "ATTACH_TO_NONE");
    lua_pushinteger(L, CLAY_ATTACH_TO_PARENT);             lua_setfield(L, -2, "ATTACH_TO_PARENT");
    lua_pushinteger(L, CLAY_ATTACH_TO_ELEMENT_WITH_ID);    lua_setfield(L, -2, "ATTACH_TO_ELEMENT_WITH_ID");
    lua_pushinteger(L, CLAY_ATTACH_TO_ROOT);               lua_setfield(L, -2, "ATTACH_TO_ROOT");

    lua_pushinteger(L, CLAY_POINTER_CAPTURE_MODE_CAPTURE);     lua_setfield(L, -2, "POINTER_CAPTURE_MODE_CAPTURE");
    lua_pushinteger(L, CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH); lua_setfield(L, -2, "POINTER_CAPTURE_MODE_PASSTHROUGH");

    lua_pushinteger(L, CLAY_CLIP_TO_NONE);               lua_setfield(L, -2, "CLIP_TO_NONE");
    lua_pushinteger(L, CLAY_CLIP_TO_ATTACHED_PARENT);    lua_setfield(L, -2, "CLIP_TO_ATTACHED_PARENT");

    Clay_CreateCommandMetatable(L);
    Clay_CreateElementBuilderMetatable(L);
    Clay_CreateFloatingBuilderMetatable(L);
    Clay_CreateTextBuilderMetatable(L);

    return 1;
}
