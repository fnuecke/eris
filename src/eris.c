/*
Eris - Heavy-duty persistence for Lua 5.2.2 - Based on Pluto
Copyright (c) 2013 by Florian Nuecke.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/* Standard library headers. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Mark us as part of the Lua core to get access to what we need. */
#define LUA_CORE

/* Public Lua headers. */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Internal Lua headers. */
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "lzio.h"

/* Eris header. */
#include "eris.h"

/*
** {===========================================================================
** Settings.
** ============================================================================
*/
/* TODO Add a way to change these via some API function. */

/* The metatable key we use to allow customized persistence for tables and
 * userdata. */
static const char *persistKey = "__persist";

/* Whether to persist debug information such as line numbers and upvalue and
 * local variable names. */
static lu_byte writeDebugInformation = 1;

/* Whether to pass the IO object (reader/writer) to the special function
 * defined in the metafield or not. This is disabled per default because it
 * mey allow Lua scripts to do more than they should. Enable this as needed. */
static lu_byte passIOToPersist = 0;

/* Generate a human readable "path" that is shown together with error messages
 * to indicate where in the object the error occurred. For example:
 * eris.persist({false, bad = setmetatable({}, {__persist = false})})
 * Will produce: main:1: attempt to persist forbidden table (root.bad)
 * This can be used for debugging, but is disabled per default due to the
 * processing and memory overhead this introduces. */
static lu_byte generatePath = 0;

/*
** ============================================================================
** Lua internals interfacing.
** ============================================================================
*/

/* Lua internals we use. We define these as macros to make it easier to swap
 * them out, should the need ever arise. For example, the later Pluto versions
 * copied these function to own files (presumably to allow building it as an
 * extra shared library). These should be all functions we use that are not
 * declared in lua.h or lauxlib.h. If there are some still directly in the code
 * they were missed and should be replaced with a macro added here instead. */
/* I'm quite sure we won't ever want to do this, because Eris needs a slightly
 * patched Lua version to be able to persist some of the library functions,
 * anyway: it needs to put the continuation C functions in the perms table. */
#define eris_setobj setobj
#define eris_setsvalue2n setsvalue2n
#define eris_setclLvalue setclLvalue
#define eris_setnilvalue setnilvalue
#define eris_gco2uv gco2uv
#define eris_obj2gco obj2gco
#define eris_incr_top incr_top
#define eris_isLua isLua
#define eris_ci_func ci_func
#define eris_savestack savestack
#define eris_restorestack restorestack
#define eris_reallocstack luaD_reallocstack
#define eris_extendCI luaE_extendCI
#define eris_findupval luaF_findupval
#define eris_newproto luaF_newproto
#define eris_newLclosure luaF_newLclosure
#define eris_newupval luaF_newupval
#define eris_reallocvector luaM_reallocvector
#define eris_newlstr luaS_newlstr
#define eris_init luaZ_init
#define eris_read luaZ_read
#define eris_buffer luaZ_buffer
#define eris_bufflen luaZ_bufflen
#define eris_sizebuffer luaZ_sizebuffer
#define eris_initbuffer luaZ_initbuffer

/* Enabled if we have a patched version of Lua (for accessing internals). */
#if 1

/* Functions in Lua libraries used to access C functions we need to add to the
 * permanents table to fully support yielded coroutines. */
extern void eris_permbaselib(lua_State *L, int forUnpersist);
extern void eris_permcorolib(lua_State *L, int forUnpersist);
extern void eris_permloadlib(lua_State *L, int forUnpersist);
extern void eris_permiolib(lua_State *L, int forUnpersist);
extern void eris_permstrlib(lua_State *L, int forUnpersist);
extern lua_Hook eris_dblib_hookf();

/* Utility macro for populating the perms table with internal C functions. */
#define populateperms(L, forUnpersist) {\
  eris_permbaselib(L, forUnpersist);\
  eris_permcorolib(L, forUnpersist);\
  eris_permloadlib(L, forUnpersist);\
  eris_permiolib(L, forUnpersist);\
  eris_permstrlib(L, forUnpersist);\
}

#else

/* Does nothing if we don't have a patched version of Lua. */
#define populateperms(L, forUnpersist) /* nothing */

#endif

/*
** ============================================================================
** Constants, settings, types and forward declarations.
** ============================================================================
*/

/* The "type" we write when we persist a value via a replacement from the
 * permanents table. This is just an arbitrary number, but it must we lower
 * than the reference offset (below) and outside the range Lua uses for its
 * types (> LUA_TOTALTAGS). */
#define ERIS_PERMANENT (LUA_TOTALTAGS + 1)

/* This is essentially the first reference we'll use. We do this to save one
 * field in our persisted data: if the value is smaller than this, the object
 * itself follows, otherwise we have a reference to an already unpersisted
 * object. Note that in the reftable the actual entries are still stored
 * starting at the first array index to have a sequence (when unpersisting). */
#define ERIS_REFERENCE_OFFSET (ERIS_PERMANENT + 1)

/* Avoids having to write the NULL all the time, plus makes it easier adding
 * a custom error message should you ever decide you want one. */
#define eris_checkstack(L, n) luaL_checkstack(L, n, NULL)

/* Used for internal consistency checks, for debugging. These are true asserts
 * in the sense that they should never fire, even for bad inputs. */
#if 0
#define eris_assert(c) assert(c)
#define eris_ifassert(e) e
#else
#define eris_assert(c) ((void)0)
#define eris_ifassert(e) /* nothing */
#endif

/* State information when persisting an object. */
typedef struct PersistInfo {
  lua_State *L;
  int refcount;
  lua_Writer writer;
  void *ud;
} PersistInfo;

/* State information when unpersisting an object. */
typedef struct UnpersistInfo {
  lua_State *L;
  int refcount;
  ZIO zio;
} UnpersistInfo;

/* Used for serialization to pick the actual reader function. */
typedef unsigned short ushort;
typedef void* voidp;

/* Type names, used for error messages. */
static const char *const kTypenames[] = {
  "nil", "boolean", "lightuserdata", "number", "string",
  "table", "function", "userdata", "thread", "proto", "upval"
};

/* }======================================================================== */

/*
** {===========================================================================
** Utility functions.
** ============================================================================
*/

/* Pushes an object into the reference table when unpersisting. This creates an
 * entry pointing from the id the object is referenced by to the object. */
static int
registerobject(UnpersistInfo *upi) {                  /* perms reftbl ... obj */
  const int reference = ++(upi->refcount);
  eris_checkstack(upi->L, 1);
  lua_pushvalue(upi->L, -1);                      /* perms reftbl ... obj obj */
  lua_rawseti(upi->L, 2, reference);                  /* perms reftbl ... obj */
  return reference;
}

/** ======================================================================== */

/* Pushes a TString* onto the stack if it holds a value, nil if it is NULL. */
static void
pushtstring(lua_State* L, TString *ts) {
  if (ts) {
    eris_setsvalue2n(L, L->top, ts);
    eris_incr_top(L);
  }
  else {
    lua_pushnil(L);
  }
}

/* Creates a copy of the string on top of the stack and sets it as the value
 * of the specified TString**. */
static void
copytstring(lua_State* L, TString **ts) {
  size_t length;
  const char *value = lua_tolstring(L, -1, &length);
  *ts = eris_newlstr(L, value, length);
}

/** ======================================================================== */

/* Pushes the specified segment to the current path, if we're generating one.
 * This supports formatting strings using Lua's formatting capabilities. */
static void
pushpath(lua_State *L, const char* fmt, ...) {       /* perms reftbl path ... */
  if (!generatePath) {
    return;
  }
  else {
    va_list argp;
    va_start(argp, fmt);
    eris_checkstack(L, 1);
    lua_pushvfstring(L, fmt, argp);              /* perms reftbl path ... str */
    va_end(argp);
    lua_rawseti(L, 3, luaL_len(L, 3) + 1);           /* perms reftbl path ... */
  }
}

/* Pops the last added segment from the current path if we're generating one. */
static void
poppath(lua_State* L) {                              /* perms reftbl path ... */
  if (!generatePath) {
    return;
  }
  eris_checkstack(L, 1);
  lua_pushnil(L);                                /* perms reftbl path ... nil */
  lua_rawseti(L, 3, luaL_len(L, 3));                 /* perms reftbl path ... */
}

/* Concatenates all current path segments into one string, pushes it and
 * returns it. This is relatively inefficient, but it's for errors only and
 * keeps the stack small, so it's better this way. */
static const char*
path(lua_State* L) {                                 /* perms reftbl path ... */
  eris_checkstack(L, 3);
  lua_pushstring(L, "");                         /* perms reftbl path ... str */
  lua_pushnil(L);                            /* perms reftbl path ... str nil */
  while (lua_next(L, 3)) {                   /* perms reftbl path ... str k v */
    lua_insert(L, -2);                       /* perms reftbl path ... str v k */
    lua_insert(L, -3);                       /* perms reftbl path ... k str v */
    lua_concat(L, 2);                          /* perms reftbl path ... k str */
    lua_insert(L, -2);                         /* perms reftbl path ... str k */
  }                                              /* perms reftbl path ... str */
  return lua_tostring(L, -1);
}

/* Used for throwing errors, automatically includes the path if available. */
static void
eris_error(lua_State* L, const char *fmt, ...) {    /* perms reftbl path? ... */
    va_list argp;
    va_start(argp, fmt);
    eris_checkstack(L, 2);
    luaL_where(L, 1);                          /* perms reftbl path ... where */
    lua_pushvfstring(L, fmt, argp);        /* perms reftbl path ... where str */
    va_end(argp);
    lua_concat(L, 2);                            /* perms reftbl path ... str */
    if (generatePath) {
      eris_checkstack(L, 3);
      lua_pushstring(L, " (");              /* perms reftbl path ... str " (" */
      path(L);                         /* perms reftbl path ... str " (" path */
      lua_pushstring(L, ")");      /* perms reftbl path ... str " (" path ")" */
      lua_concat(L, 4);
    }
    lua_error(L);
}

/* }======================================================================== */

/*
** {===========================================================================
** Persist and unpersist.
** ============================================================================
*/

/* I have macros and I'm not afraid to use them! These are highly situational
 * and assume a PersistInfo* named 'pi' is available for the writing ones, and
 * a UnpersistInfo* named 'upi' is available for the reading ones. */

/* Writes a raw memory block with the specified size. */
#define WRITE_RAW(value, size) {\
  if (pi->writer(pi->L, (value), (size), pi->ud)) \
    eris_error(pi->L, "could not write data"); }

/* Writes a typed array with the specified length. */
#define WRITE(value, length, type) \
  WRITE_RAW((void*)(value), (length) * sizeof(type))

/* Writes a single value with the specified type. The value is cast to the
 * specified type if necessary before being written. */
#define WRITE_VALUE(value, type) {\
  type value_ = (type)(value); WRITE(&value_, 1, type); }

/** ======================================================================== */

/* Reads a raw block of memory with the specified size. */
#define READ_RAW(value, size) {\
  if (eris_read(&upi->zio, (value), (size))) \
    eris_error(upi->L, "could not read data"); }

/* Reads a typed array with the specified length. */
#define READ(value, length, type) \
  READ_RAW((void*)(value), (length) * sizeof(type))

/* Reads a single value with the specified type. */
#define READ_VALUE(type) read_##type(upi)

/* These are used by READ() and are the actual callbacks for the supported
 * types (i.e. the ones we need). It's just a choice of style. */
#define READ_VALUE_T(type) static type read_##type(UnpersistInfo *upi) {\
  type r; READ(&r, 1, type); return r; }
READ_VALUE_T(lu_byte);
READ_VALUE_T(short);
READ_VALUE_T(ushort);
READ_VALUE_T(int);
READ_VALUE_T(size_t);
READ_VALUE_T(lua_Number);
READ_VALUE_T(voidp);
READ_VALUE_T(ptrdiff_t);
#undef READ_VALUE_T

/** ======================================================================== */

/* Forward declarations for recursively called top-level functions. */
static void persist_keyed(PersistInfo*, int);
static void persist(PersistInfo*);
static void unpersist(UnpersistInfo*);

/*
** ============================================================================
** Simple types.
** ============================================================================
*/

static void
p_boolean(PersistInfo *pi) {                                      /* ... bool */
  WRITE_VALUE(lua_toboolean(pi->L, -1), lu_byte);
}

static void
u_boolean(UnpersistInfo *upi) {                                        /* ... */
  eris_checkstack(upi->L, 1);
  lua_pushboolean(upi->L, READ_VALUE(lu_byte));                   /* ... bool */

  eris_assert(lua_type(upi->L, -1) == LUA_TBOOLEAN);
}

/** ======================================================================== */

static void
p_pointer(PersistInfo *pi) {                                    /* ... ludata */
  WRITE_VALUE(lua_touserdata(pi->L, -1), voidp);
}

static void
u_pointer(UnpersistInfo *upi) {                                        /* ... */
  eris_checkstack(upi->L, 1);
  lua_pushlightuserdata(upi->L, READ_VALUE(voidp));             /* ... ludata */

  eris_assert(lua_type(upi->L, -1) == LUA_TLIGHTUSERDATA);
}

/** ======================================================================== */

static void
p_number(PersistInfo *pi) {                                        /* ... num */
  WRITE_VALUE(lua_tonumber(pi->L, -1), lua_Number);
}

static void
u_number(UnpersistInfo *upi) {                                         /* ... */
  eris_checkstack(upi->L, 1);
  lua_pushnumber(upi->L, READ_VALUE(lua_Number));                  /* ... num */

  eris_assert(lua_type(upi->L, -1) == LUA_TNUMBER);
}

/** ======================================================================== */

static void
p_string(PersistInfo *pi) {                                        /* ... str */
  size_t length;
  const char *value = lua_tolstring(pi->L, -1, &length);
  WRITE_VALUE(length, size_t);
  WRITE(value, length, char);
}

static void
u_string(UnpersistInfo *upi) {                                         /* ... */
  eris_checkstack(upi->L, 2);
  {
    /* TODO Can we avoid this copy somehow? (Without it getting too nasty) */
    const size_t length = READ_VALUE(size_t);
    char *value = lua_newuserdata(upi->L, length * sizeof(char));  /* ... tmp */
    READ(value, length, char);
    lua_pushlstring(upi->L, value, length);                    /* ... tmp str */
    lua_replace(upi->L, -2);                                       /* ... str */
  }
  registerobject(upi);

  eris_assert(lua_type(upi->L, -1) == LUA_TSTRING);
}

/*
** ============================================================================
** Tables and userdata.
** ============================================================================
*/

static void
p_metatable(PersistInfo *pi) {                                     /* ... obj */
  eris_checkstack(pi->L, 1);
  pushpath(pi->L, "@metatable");
  if (!lua_getmetatable(pi->L, -1)) {                          /* ... obj mt? */
    lua_pushnil(pi->L);                                        /* ... obj nil */
  }                                                         /* ... obj mt/nil */
  persist(pi);                                              /* ... obj mt/nil */
  lua_pop(pi->L, 1);                                               /* ... obj */
  poppath(pi->L);
}

static void
u_metatable(UnpersistInfo *upi) {                                  /* ... tbl */
  eris_checkstack(upi->L, 1);
  pushpath(upi->L, "@metatable");
  unpersist(upi);                                          /* ... tbl mt/nil? */
  if (lua_istable(upi->L, -1)) {                                /* ... tbl mt */
    lua_setmetatable(upi->L, -2);                                  /* ... tbl */
  }
  else if (lua_isnil(upi->L, -1)) {                            /* ... tbl nil */
    lua_pop(upi->L, 1);                                            /* ... tbl */
  }
  else {                                                            /* tbl :( */
    eris_error(upi->L, "bad metatable, not nil or table");
  }
  poppath(upi->L);
}

/** ======================================================================== */

static void
p_literaltable(PersistInfo *pi) {                                  /* ... tbl */
  eris_checkstack(pi->L, 3);

  /* Persist all key / value pairs. */
  lua_pushnil(pi->L);                                          /* ... tbl nil */
  while (lua_next(pi->L, -2)) {                                /* ... tbl k v */
    lua_pushvalue(pi->L, -2);                                /* ... tbl k v k */

    if (generatePath) {
      if (lua_type(pi->L, -1) == LUA_TSTRING) {
        const char *key = lua_tostring(pi->L, -1);
        pushpath(pi->L, ".%s", key);
      }
      else {
        const char *key = luaL_tolstring(pi->L, -1, NULL);
        pushpath(pi->L, "[%s]", key);
        lua_pop(pi->L, 1);
      }
    }

    persist(pi);                                             /* ... tbl k v k */
    lua_pop(pi->L, 1);                                         /* ... tbl k v */
    persist(pi);                                               /* ... tbl k v */
    lua_pop(pi->L, 1);                                           /* ... tbl k */

    poppath(pi->L);
  }                                                                /* ... tbl */

  /* Terminate list. */
  lua_pushnil(pi->L);                                          /* ... tbl nil */
  persist(pi);                                                 /* ... tbl nil */
  lua_pop(pi->L, 1);                                               /* ... tbl */

  p_metatable(pi);
}

static void
u_literaltable(UnpersistInfo *upi) {                                   /* ... */
  eris_checkstack(upi->L, 3);

  lua_newtable(upi->L);                                            /* ... tbl */

  /* Preregister table for handling of cycles (keys, values or metatable). */
  registerobject(upi);

  /* Unpersist all key / value pairs. */
  for (;;) {
    pushpath(upi->L, "@key");
    unpersist(upi);                                        /* ... tbl key/nil */
    poppath(upi->L);
    if (lua_isnil(upi->L, -1)) {                               /* ... tbl nil */
      lua_pop(upi->L, 1);                                          /* ... tbl */
      break;
    }                                                          /* ... tbl key */

    if (generatePath) {
      if (lua_type(upi->L, -1) == LUA_TSTRING) {
        const char *key = lua_tostring(upi->L, -1);
        pushpath(upi->L, ".%s", key);
      }
      else {
        const char *key = luaL_tolstring(upi->L, -1, NULL);
        pushpath(upi->L, "[%s]", key);
        lua_pop(upi->L, 1);
      }
    }

    unpersist(upi);                                     /* ... tbl key value? */
    if (!lua_isnil(upi->L, -1)) {                        /* ... tbl key value */
      lua_rawset(upi->L, -3);                                      /* ... tbl */
    }
    else {
      eris_error(upi->L, "bad table value, got a nil value");
    }

    poppath(upi->L);
  }

  u_metatable(upi);                                                /* ... tbl */
}

/** ======================================================================== */

static void
p_literaluserdata(PersistInfo *pi) {                             /* ... udata */
  const size_t size = lua_rawlen(pi->L, -1);
  const void *value = lua_touserdata(pi->L, -1);
  WRITE_VALUE(size, size_t);
  WRITE_RAW(value, size);
  p_metatable(pi);                                               /* ... udata */
}

static void
u_literaluserdata(UnpersistInfo *upi) {                                /* ... */
  eris_checkstack(upi->L, 1);
  {
    size_t size = READ_VALUE(size_t);
    void *value = lua_newuserdata(upi->L, size);                 /* ... udata */
    READ_RAW(value, size);                                       /* ... udata */
  }
  registerobject(upi);
  u_metatable(upi);
}

/** ======================================================================== */

typedef void (*PersistCallback) (PersistInfo*);
typedef void (*UnpersistCallback) (UnpersistInfo*);

static void
p_special(PersistInfo *pi, PersistCallback literal) {              /* ... obj */
  int allow = (lua_type(pi->L, -1) == LUA_TTABLE);
  eris_checkstack(pi->L, 4);

  /* Check whether we should persist literally, or via the metafunction. */
  if (lua_getmetatable(pi->L, -1)) {                            /* ... obj mt */
    lua_pushstring(pi->L, persistKey);                     /* ... obj mt pkey */
    lua_rawget(pi->L, -2);                             /* ... obj mt persist? */
    switch (lua_type(pi->L, -1)) {
      /* No entry, act according to default. */
      case LUA_TNIL:                                        /* ... obj mt nil */
        lua_pop(pi->L, 2);                                         /* ... obj */
        break;

      /* Boolean value, tells us whether allowed or not. */
      case LUA_TBOOLEAN:                                   /* ... obj mt bool */
        allow = lua_toboolean(pi->L, -1);
        lua_pop(pi->L, 2);                                         /* ... obj */
        break;

      /* Function value, call it and don't persist literally. */
      case LUA_TFUNCTION:                                  /* ... obj mt func */
        lua_replace(pi->L, -2);                               /* ... obj func */
        lua_pushvalue(pi->L, -2);                         /* ... obj func obj */

        if (passIOToPersist) {
          lua_pushlightuserdata(pi->L, pi->writer);/* ... obj func obj writer */
          lua_pushlightuserdata(pi->L, pi->ud); /* ... obj func obj writer ud */
          lua_call(pi->L, 3, 1);                             /* ... obj func? */
        }
        else {
          lua_call(pi->L, 1, 1);                             /* ... obj func? */
        }
        if (!lua_isfunction(pi->L, -1)) {                       /* ... obj :( */
          eris_error(pi->L, "%s did not return a function", persistKey);
        }                                                     /* ... obj func */

        /* Special persistence, call this function when unpersisting. */
        WRITE_VALUE(true, lu_byte);
        persist(pi);                                          /* ... obj func */
        lua_pop(pi->L, 1);                                         /* ... obj */
        return;
      default:                                               /* ... obj mt :( */
        eris_error(pi->L, "%d not nil, boolean, or function", persistKey);
        return; /* not reached */
    }
  }

  if (allow) {
    /* Not special but literally persisted object. */
    WRITE_VALUE(0, lu_byte);
    literal(pi);                                                   /* ... obj */
  }
  else if (lua_type(pi->L, -1) == LUA_TTABLE) {
    eris_error(pi->L, "attempt to persist forbidden table");
  }
  else {
    eris_error(pi->L, "literally persisting userdata is disabled by default");
  }
}

static void
u_special(UnpersistInfo *upi, int type, UnpersistCallback literal) {   /* ... */
  eris_checkstack(upi->L, passIOToPersist ? 2 : 1);
  if (READ_VALUE(lu_byte)) {
    int reference;
    /* Reserve entry in the reftable before unpersisting the function to keep
     * the reference order intact. We can set this to nil at first, because
     * there's no way the special function would access this. */
    lua_pushnil(upi->L);                                           /* ... nil */
    reference = registerobject(upi);
    lua_pop(upi->L, 1);                                                /* ... */
    /* Increment reference counter by one to compensate for the increment when
     * persisting a special object. */
    unpersist(upi);                                            /* ... spfunc? */
    if (!lua_isfunction(upi->L, -1)) {                              /* ... :( */
      eris_error(upi->L, "invalid restore function");
    }                                                           /* ... spfunc */

    if (passIOToPersist) {
      lua_pushlightuserdata(upi->L, &upi->zio);             /* ... spfunc zio */
      lua_call(upi->L, 1, 1);                                     /* ... obj? */
    } else {
      lua_call(upi->L, 0, 1);                                     /* ... obj? */
    }

    if (lua_type(upi->L, -1) != type) {                             /* ... :( */
      eris_error(upi->L, "bad unpersist function (%s expected, returned %s)",
                 kTypenames[type], kTypenames[lua_type(upi->L, -1)]);
    }                                                              /* ... obj */

    /* Update the reftable entry. */
    lua_pushvalue(upi->L, -1);                                 /* ... obj obj */
    lua_rawseti(upi->L, 2, reference);                             /* ... obj */
  }
  else {
    literal(upi);                                                  /* ... obj */
  }
}

/** ======================================================================== */

static void
p_table(PersistInfo *pi) {                                         /* ... tbl */
  p_special(pi, p_literaltable);                                   /* ... tbl */
}

static void
u_table(UnpersistInfo *upi) {                                          /* ... */
  u_special(upi, LUA_TTABLE, u_literaltable);                      /* ... tbl */

  eris_assert(lua_type(upi->L, -1) == LUA_TTABLE);
}

/** ======================================================================== */

static void
p_userdata(PersistInfo *pi) {                       /* perms reftbl ... udata */
  p_special(pi, p_literaluserdata);
}

static void
u_userdata(UnpersistInfo *upi) {                                       /* ... */
  u_special(upi, LUA_TUSERDATA, u_literaluserdata);              /* ... udata */

  eris_assert(lua_type(upi->L, -1) == LUA_TUSERDATA);
}

/*
** ============================================================================
** Closures and threads.
** ============================================================================
*/

/* We track the actual upvalues themselves by pushing their "id" (meaning a
 * pointer to them) as lightuserdata to the reftable. This is safe because
 * lightuserdata will not normally end up in their, because simple value types
 * are always persisted directly (because that'll be just as large, memory-
 * wise as when pointing to the first instance). Same for protos.
 */

static void
p_proto(PersistInfo *pi) {                                       /* ... proto */
  int i;
  const Proto *p = lua_touserdata(pi->L, -1);
  eris_checkstack(pi->L, 2);

  /* Write general information. */
  WRITE_VALUE(p->linedefined, int);
  WRITE_VALUE(p->lastlinedefined, int);
  WRITE_VALUE(p->numparams, lu_byte);
  WRITE_VALUE(p->is_vararg, lu_byte);
  WRITE_VALUE(p->maxstacksize, lu_byte);

  /* Write byte code. */
  WRITE_VALUE(p->sizecode, int);
  WRITE(p->code, p->sizecode, Instruction);

  /* Write constants. */
  WRITE_VALUE(p->sizek, int);
  pushpath(pi->L, ".constants");
  for (i = 0; i < p->sizek; ++i) {
    pushpath(pi->L, "[%d]", i);
    eris_setobj(pi->L, pi->L->top++, &p->k[i]);          /* ... lcl proto obj */
    persist(pi);                                         /* ... lcl proto obj */
    lua_pop(pi->L, 1);                                       /* ... lcl proto */
    poppath(pi->L);
  }
  poppath(pi->L);

  /* Write child protos. */
  WRITE_VALUE(p->sizep, int);
  pushpath(pi->L, ".protos");
  for (i = 0; i < p->sizep; ++i) {
    pushpath(pi->L, "[%d]", i);
    lua_pushlightuserdata(pi->L, p->p[i]);             /* ... lcl proto proto */
    lua_pushvalue(pi->L, -1);                    /* ... lcl proto proto proto */
    persist_keyed(pi, LUA_TPROTO);                     /* ... lcl proto proto */
    lua_pop(pi->L, 1);                                       /* ... lcl proto */
    poppath(pi->L);
  }
  poppath(pi->L);

  /* Write upvalues. */
  WRITE_VALUE(p->sizeupvalues, int);
  for (i = 0; i < p->sizeupvalues; ++i) {
    WRITE_VALUE(p->upvalues[i].instack, lu_byte);
    WRITE_VALUE(p->upvalues[i].idx, lu_byte);
  }

  /* If we don't have to persist debug information skip the rest. */
  WRITE_VALUE(writeDebugInformation, lu_byte);
  if (!writeDebugInformation) {
    return;
  }

  /* Write function source code. */
  pushtstring(pi->L, p->source);                      /* ... lcl proto source */
  persist(pi);                                        /* ... lcl proto source */
  lua_pop(pi->L, 1);                                         /* ... lcl proto */

  /* Write line information. */
  WRITE_VALUE(p->sizelineinfo, int);
  WRITE(p->lineinfo, p->sizelineinfo, int);

  /* Write locals info. */
  WRITE_VALUE(p->sizelocvars, int);
  pushpath(pi->L, ".locvars");
  for (i = 0; i < p->sizelocvars; ++i) {
    pushpath(pi->L, "[%d]", i);
    WRITE_VALUE(p->locvars[i].startpc, int);
    WRITE_VALUE(p->locvars[i].endpc, int);
    pushtstring(pi->L, p->locvars[i].varname);       /* ... lcl proto varname */
    persist(pi);                                     /* ... lcl proto varname */
    lua_pop(pi->L, 1);                                       /* ... lcl proto */
    poppath(pi->L);
  }
  poppath(pi->L);

  /* Write upvalue names. */
  pushpath(pi->L, ".upvalnames");
  for (i = 0; i < p->sizeupvalues; ++i) {
    pushpath(pi->L, "[%d]", i);
    pushtstring(pi->L, p->upvalues[i].name);            /* ... lcl proto name */
    persist(pi);                                        /* ... lcl proto name */
    lua_pop(pi->L, 1);                                       /* ... lcl proto */
    poppath(pi->L);
  }
  poppath(pi->L);
}

static void
u_proto(UnpersistInfo *upi) {                                    /* ... proto */
  int i, n;
  Proto *p = lua_touserdata(upi->L, -1);
  eris_assert(p);

  eris_checkstack(upi->L, 1);

  /* Preregister proto for handling of cycles (probably impossible, but
   * maybe via the constants of the proto... not worth taking the risk). */
  registerobject(upi);

  /* Read general information. */
  p->linedefined = READ_VALUE(int);
  p->lastlinedefined = READ_VALUE(int);
  p->numparams = READ_VALUE(lu_byte);
  p->is_vararg = READ_VALUE(lu_byte);
  p->maxstacksize= READ_VALUE(lu_byte);

  /* Read byte code. */
  p->sizecode = READ_VALUE(int);
  eris_reallocvector(upi->L, p->code, 0, p->sizecode, Instruction);
  READ(p->code, p->sizecode, Instruction);

  /* Read constants. */
  p->sizek = READ_VALUE(int);
  eris_reallocvector(upi->L, p->k, 0, p->sizek, TValue);
  pushpath(upi->L, ".constants");
  for (i = 0, n = p->sizek; i < n; ++i) {
    pushpath(upi->L, "[%d]", i);
    unpersist(upi);                                          /* ... proto obj */
    eris_setobj(upi->L, &p->k[i], upi->L->top - 1);
    lua_pop(upi->L, 1);                                          /* ... proto */
    poppath(upi->L);
  }
  poppath(upi->L);

  /* Read child protos. */
  p->sizep = READ_VALUE(int);
  eris_reallocvector(upi->L, p->p, 0, p->sizep, Proto*);
  pushpath(upi->L, ".protos");
  for (i = 0, n = p->sizep; i < n; ++i) {
    Proto *cp;
    pushpath(upi->L, "[%d]", i);
    p->p[i] = eris_newproto(upi->L);
    lua_pushlightuserdata(upi->L, p->p[i]);               /* ... proto nproto */
    unpersist(upi);                         /* ... proto nproto nproto/oproto */
    cp = lua_touserdata(upi->L, -1);
    if (cp != p->p[i]) {                           /* ... proto nproto oproto */
      /* Just overwrite it, GC will clean this up. */
      p->p[i] = cp;
    }
    lua_pop(upi->L, 2);                                          /* ... proto */
    poppath(upi->L);
  }
  poppath(upi->L);

  /* Read upvalues. */
  p->sizeupvalues = READ_VALUE(int);
  eris_reallocvector(upi->L, p->upvalues, 0, p->sizeupvalues, Upvaldesc);
  for (i = 0, n = p->sizeupvalues; i < n; ++i) {
    p->upvalues[i].name = NULL;
    p->upvalues[i].instack = READ_VALUE(lu_byte);
    p->upvalues[i].idx = READ_VALUE(lu_byte);
  }

  /* Read debug information if any is present. */
  if (!READ_VALUE(lu_byte)) {
    return;
  }

  /* Read function source code. */
  unpersist(upi);                                            /* ... proto str */
  copytstring(upi->L, &p->source);
  lua_pop(upi->L, 1);                                            /* ... proto */

  /* Read line information. */
  p->sizelineinfo = READ_VALUE(int);
  eris_reallocvector(upi->L, p->lineinfo, 0, p->sizelineinfo, int);
  READ(p->lineinfo, p->sizelineinfo, int);

  /* Read locals info. */
  p->sizelocvars = READ_VALUE(int);
  eris_reallocvector(upi->L, p->locvars, 0, p->sizelocvars, LocVar);
  pushpath(upi->L, ".locvars");
  for (i = 0, n = p->sizelocvars; i < n; ++i) {
    pushpath(upi->L, "[%d]", i);
    p->locvars[i].startpc = READ_VALUE(int);
    p->locvars[i].endpc = READ_VALUE(int);
    unpersist(upi);                                          /* ... proto str */
    copytstring(upi->L, &p->locvars[i].varname);
    lua_pop(upi->L, 1);                                          /* ... proto */
    poppath(upi->L);
  }
  poppath(upi->L);

  /* Read upvalue names. */
  pushpath(upi->L, ".upvalnames");
  for (i = 0, n = p->sizeupvalues; i < n; ++i) {
    pushpath(upi->L, "[%d]", i);
    unpersist(upi);                                          /* ... proto str */
    copytstring(upi->L, &p->upvalues[i].name);
    lua_pop(upi->L, 1);                                          /* ... proto */
    poppath(upi->L);
  }
  poppath(upi->L);
  lua_pushvalue(upi->L, -1);                               /* ... proto proto */

  eris_assert(lua_type(upi->L, -1) == LUA_TLIGHTUSERDATA);
}

/** ======================================================================== */

static void
p_upval(PersistInfo *pi) {                                         /* ... obj */
  persist(pi);                                                     /* ... obj */
}

static void
u_upval(UnpersistInfo *upi) {                                          /* ... */
  eris_checkstack(upi->L, 2);

  /* Create the table we use to store the pointer to the actual upval (1), the
   * value of the upval (2) and any pointers to the pointer to the upval (3+).*/
  lua_createtable(upi->L, 3, 0);                                   /* ... tbl */
  registerobject(upi);
  unpersist(upi);                                              /* ... tbl obj */
  lua_rawseti(upi->L, -2, 1);                                      /* ... tbl */

  eris_assert(lua_type(upi->L, -1) == LUA_TTABLE);
}

/** ======================================================================== */

/* For Lua closures we write the upvalue ID, which is usually the memory
 * address at which it is stored. This is used to tell which upvalues are
 * identical when unpersisting. */
/* In either case we store the upvale *values*, i.e. the actual objects they
 * point to. As in Pluto, we will restore any upvalues of Lua closures as
 * closed as first, i.e. the upvalue will store the TValue itself. When
 * loading a thread containing the upvalue (meaning it's the actual owner of
 * the upvalue) we open it, i.e. we point it to the thread's upvalue list.
 * For C closures, upvalues are always closed. */
static void
p_closure(PersistInfo *pi) {                         /* perms reftbl ... func */
  size_t nup;
  eris_checkstack(pi->L, 2);
  switch (ttype(pi->L->top - 1)) {
    case LUA_TLCF: /* light C function */
      /* We cannot persist these, they have to be handled via the permtable. */
      eris_error(pi->L, "Attempt to persist a light C function (%p)",
                 lua_tocfunction(pi->L, -1));
      return; /* not reached */
    case LUA_TCCL: /* C closure */ {                  /* perms reftbl ... ccl */
      CClosure *cl = clCvalue(pi->L->top - 1);
      /* Mark it as a C closure. */
      WRITE_VALUE(true, lu_byte);
      /* Write the upvalue count first, since we have to know it when creating
       * a new closure when unpersisting. */
      WRITE_VALUE(cl->nupvalues, lu_byte);

      /* We can only persist these if the underlying C function is in the
       * permtable. So we try to persist it first as a light C function. If it
       * isn't in the permtable that'll cause an error (in the case above). */
      lua_pushcclosure(pi->L, lua_tocfunction(pi->L, -1), 0);
                                                /* perms reftbl ... ccl cfunc */
      persist(pi);                              /* perms reftbl ... ccl cfunc */
      lua_pop(pi->L, 1);                              /* perms reftbl ... ccl */

      /* Persist the upvalues. Since for C closures all upvalues are always
       * closed we can just write the actual values. */
      pushpath(pi->L, ".upvalues");
      for (nup = 1; nup <= cl->nupvalues; ++nup) {
        pushpath(pi->L, "[%d]", nup);
        lua_getupvalue(pi->L, -1, nup);           /* perms reftbl ... ccl obj */
        persist(pi);                              /* perms reftbl ... ccl obj */
        lua_pop(pi->L, 1);                            /* perms reftbl ... ccl */
        poppath(pi->L);
      }
      poppath(pi->L);
      break;
    }
    case LUA_TLCL: /* Lua function */ {               /* perms reftbl ... lcl */
      LClosure *cl = clLvalue(pi->L->top - 1);
      /* Mark it as a Lua closure. */
      WRITE_VALUE(false, lu_byte);
      /* Write the upvalue count first, since we have to know it when creating
       * a new closure when unpersisting. */
      WRITE_VALUE(cl->nupvalues, lu_byte);

      /* Persist the function's prototype. Pass the proto as a parameter to
       * p_proto so that it can access it and register it in the ref table. */
      pushpath(pi->L, ".proto");
      lua_pushlightuserdata(pi->L, cl->p);      /* perms reftbl ... lcl proto */
      lua_pushvalue(pi->L, -1);           /* perms reftbl ... lcl proto proto */
      persist_keyed(pi, LUA_TPROTO);            /* perms reftbl ... lcl proto */
      lua_pop(pi->L, 1);                              /* perms reftbl ... lcl */
      poppath(pi->L);

      /* Persist the upvalues. We pretend to write these as their own type,
       * to get proper identity preservation. We also pass them as a parameter
       * to p_upval so it can register the upvalue in the reference table. */
      pushpath(pi->L, ".upvalues");
      for (nup = 1; nup <= cl->nupvalues; ++nup) {
        const char *name = lua_getupvalue(pi->L, -1, nup);
                                                  /* perms reftbl ... lcl obj */
        pushpath(pi->L, ".%s", name);
        lua_pushlightuserdata(pi->L, lua_upvalueid(pi->L, -2, nup));
                                               /* perms reftbl ... lcl obj id */
        persist_keyed(pi, LUA_TUPVAL);            /* perms reftbl ... lcl obj */
        lua_pop(pi->L, 1);                           /* perms reftble ... lcl */
        poppath(pi->L);
      }
      poppath(pi->L);
      break;
    }
    default:
      eris_error(pi->L, "Attempt to persist unknown function type");
      return; /* not reached */
  }
}

static void
u_closure(UnpersistInfo *upi) {                                        /* ... */
  size_t nup;
  lu_byte isCClosure = READ_VALUE(lu_byte);
  lu_byte nups = READ_VALUE(lu_byte);
  if (isCClosure) {
    lua_CFunction f;

    /* nups is guaranteed to be >= 1, otherwise it'd be a light C function. */
    eris_checkstack(upi->L, nups);

    /* Read the C function from the permanents table. */
    unpersist(upi);                                              /* ... cfunc */
    f = lua_tocfunction(upi->L, -1);
    lua_pop(upi->L, 1);                                                /* ... */

    /* Now this is a little roundabout, but we want to create the closure
     * before unpersisting the actual upvalues to avoid cycles. So we have to
     * create it with all nil first, then fill the upvalues in afterwards. */
    for (nup = 1; nup <= nups; ++nup) {
      lua_pushnil(upi->L);                         /* ... nil[1] ... nil[nup] */
    }
    lua_pushcclosure(upi->L, f, nups);                             /* ... ccl */

    /* Preregister closure for handling of cycles (upvalues). */
    registerobject(upi);

    /* Unpersist actual upvalues. */
    pushpath(upi->L, ".upvalues");
    for (nup = 1; nup <= nups; ++nup) {
      pushpath(upi->L, "[%d]", nup);
      unpersist(upi);                                          /* ... ccl obj */
      lua_setupvalue(upi->L, -2, nup);                             /* ... ccl */
      poppath(upi->L);
    }
    poppath(upi->L);
  }
  else {
    Closure *cl;
    Proto *p;

    eris_checkstack(upi->L, 4);

    /* Create closure and anchor it on the stack (avoid collection via GC). */
    cl = eris_newLclosure(upi->L, nups);
    eris_setclLvalue(upi->L, upi->L->top, cl);                     /* ... lcl */
    eris_incr_top(upi->L);

    /* Preregister closure for handling of cycles (upvalues). */
    registerobject(upi);

    /* Read prototype. In general, we create protos (and upvalues) before
     * trying to read them and pass a pointer to the instance along to the
     * unpersist function. This way the instance is safely hooked up to an
     * object, so we don't have to worry about it getting GCed. */
    pushpath(upi->L, ".proto");
    cl->l.p = eris_newproto(upi->L);
    /* Push the proto into which to unpersist as a parameter to u_proto. */
    lua_pushlightuserdata(upi->L, cl->l.p);                 /* ... lcl nproto */
    unpersist(upi);                           /* ... lcl nproto nproto/oproto */
    eris_assert(lua_type(upi->L, -1) == LUA_TLIGHTUSERDATA);
    /* The proto we have now may differ, if we already unpersisted it before.
     * In that case we now have a reference to the originally unpersisted
     * proto so we'll use that. */
    p = lua_touserdata(upi->L, -1);
    if (p != cl->l.p) {                              /* ... lcl nproto oproto */
      /* Just overwrite the old one, GC will clean this up. */
      cl->l.p = p;
    }
    lua_pop(upi->L, 2);                                            /* ... lcl */
    eris_assert(cl->l.p->sizeupvalues == nups);
    poppath(upi->L);

    /* Unpersist all upvalues. */
    pushpath(upi->L, ".upvalues");
    for (nup = 1; nup <= nups; ++nup) {
      UpVal **uv = &cl->l.upvals[nup - 1];
      /* Get the actual name of the upvalue, if possible. */
      if (p->upvalues[nup - 1].name) {
        pushpath(upi->L, "[%s]", getstr(p->upvalues[nup - 1].name));
      }
      else {
        pushpath(upi->L, "[%d]", nup);
      }
      unpersist(upi);                                          /* ... lcl tbl */
      eris_assert(lua_type(upi->L, -1) == LUA_TTABLE);
      lua_rawgeti(upi->L, -1, 2);                    /* ... lcl tbl upval/nil */
      if (lua_isnil(upi->L, -1)) {                         /* ... lcl tbl nil */
        lua_pop(upi->L, 1);                                    /* ... lcl tbl */
        *uv = eris_newupval(upi->L);
        lua_pushlightuserdata(upi->L, *uv);              /* ... lcl tbl upval */
        lua_rawseti(upi->L, -2, 2);                            /* ... lcl tbl */
      }
      else {                                             /* ... lcl tbl upval */
        eris_assert(lua_type(upi->L, -1) == LUA_TLIGHTUSERDATA);
        *uv = lua_touserdata(upi->L, -1);
        lua_pop(upi->L, 1);                                    /* ... lcl tbl */
      }

      /* Always update the value of the upvalue - if we had a cycle, it might
       * have been incorrectly initialized to nil. */
      lua_rawgeti(upi->L, -1, 1);                          /* ... lcl tbl obj */
      eris_setobj(upi->L, &(*uv)->u.value, upi->L->top - 1);
      lua_pop(upi->L, 1);                                      /* ... lcl tbl */

      /* Add our reference to the upvalue to the list, for pointer patching
       * if we have to open the upvalue in u_thread. */
      lua_pushlightuserdata(upi->L, uv);                /* ... lcl tbl upvalp */
      if (luaL_len(upi->L, -2) >= 2) {
        lua_rawseti(upi->L, -2, luaL_len(upi->L, -2) + 1);     /* ... lcl tbl */
      }
      else {
        int i;
        /* Find where to insert. This can happen if we have cycles, in which
         * case the table is not fully initialized at this point, i.e. the
         * value is not in it, yet (we work around that by always setting it).*/
        for (i = 3;; ++i) {
          lua_rawgeti(upi->L, -2, i);        /* ... lcl tbl upvalp upvalp/nil */
          if (lua_isnil(upi->L, -1)) {              /* ... lcl tbl upvalp nil */
            lua_pop(upi->L, 1);                         /* ... lcl tbl upvalp */
            lua_rawseti(upi->L, -2, i);                        /* ... lcl tbl */
            break;
          }
          else {
            lua_pop(upi->L, 1);                         /* ... lcl tbl upvalp */
          }
        }
      }
      lua_pop(upi->L, 1);                                          /* ... lcl */
      poppath(upi->L);
    }
    poppath(upi->L);

    luaC_barrierproto(upi->L, p, cl);
    p->cache = cl; /* save it on cache for reuse, see lvm.c:416 */
  }

  eris_assert(lua_type(upi->L, -1) == LUA_TFUNCTION);
}

/** ======================================================================== */

static void
p_thread(PersistInfo *pi) {                                     /* ... thread */
  lua_State* thread = lua_tothread(pi->L, -1);
  size_t level;
  StkId o;
  CallInfo *ci;
  UpVal *uv;

  eris_checkstack(pi->L, 2);

  /* We cannot persist any running threads, because by definition we *are* that
   * running thread. And we use the stack. So yeah, really not a good idea. */
  if (thread == pi->L) {
    eris_error(pi->L, "cannot persist currently running thread");
    return; /* not reached */
  }

  /* If the thread isn't running this must be the default value, which is 1. */
  eris_assert(thread->nny == 1);

  /* Error jump info should only be set while thread is running. */
  eris_assert(thread->errorJmp == NULL);
  eris_assert(thread->errfunc == 0);

  /* thread->oldpc always seems to be uninitialized, at least gdb always shows
   * it as 0xbaadfood when I set a breakpoint here. */

  /* Write general information. */
  WRITE_VALUE(thread->status, lu_byte);
  WRITE_VALUE(thread->nCcalls, ushort);
  WRITE_VALUE(thread->allowhook, lu_byte);

  /* Hooks are not supported, bloody can of worms, those.
  WRITE_VALUE(thread->hookmask, lu_byte);
  WRITE_VALUE(thread->basehookcount, int);
  WRITE_VALUE(thread->hookcount, int); */

  if (thread->hook) {
    /* Warn that hooks are not persisted? */
  }

  /* Persist the stack. Save the total size and used space first. */
  WRITE_VALUE(thread->stacksize, int);
  WRITE_VALUE(thread->top - thread->stack, size_t);

  /* The Lua stack looks like this:
   * stack ... top ... stack_last
   * Where stack <= top <= stack_last, and "top" actually being the first free
   * element, i.e. there's nothing stored there. So we stop one below that. */
  pushpath(pi->L, ".stack");
  lua_pushnil(pi->L);                                       /* ... thread nil */
  level = 0;
  for (o = thread->stack; o < thread->top; ++o) {
    pushpath(pi->L, "[%d]", level++);
    eris_setobj(pi->L, pi->L->top - 1, o);                  /* ... thread obj */
    persist(pi);                                            /* ... thread obj */
    poppath(pi->L);
  }
  lua_pop(pi->L, 1);                                            /* ... thread */
  poppath(pi->L);

  /* Write call information (stack frames). In 5.2 CallInfo is stored in a
   * linked list that originates in thead.base_ci. Upon initialization the
   * thread.ci is set to thread.base_ci. During thread calls this is extended
   * and always represents the tail of the linked list. */
  pushpath(pi->L, ".callinfo");
  level = 0;
  eris_assert(&thread->base_ci != thread->ci->next);
  for (ci = &thread->base_ci; ci != thread->ci->next; ci = ci->next) {
    pushpath(pi->L, "[%d]", level++);
    WRITE_VALUE(eris_savestack(thread, ci->func), size_t);
    WRITE_VALUE(eris_savestack(thread, ci->top), size_t);
    WRITE_VALUE(ci->nresults, short);
    WRITE_VALUE(ci->callstatus, lu_byte);
    WRITE_VALUE(ci->extra, ptrdiff_t);

    eris_assert(eris_isLua(ci) || (ci->callstatus & CIST_TAIL) == 0);
    if (ci->callstatus & CIST_HOOKYIELD) {
      eris_error(pi->L, "cannot persist yielded hooks");
    }

    if (eris_isLua(ci)) {
      const LClosure *lcl = eris_ci_func(ci);
      WRITE_VALUE(eris_savestack(thread, ci->u.l.base), size_t);
      WRITE_VALUE(ci->u.l.savedpc - lcl->p->code, size_t);
    }
    else {
      WRITE_VALUE(ci->u.c.status, lu_byte);

      /* These are only used while a thread is being executed:
      WRITE_VALUE(ci->u.c.old_errfunc, ptrdiff_t);
      WRITE_VALUE(ci->u.c.old_allowhook, lu_byte); */

      /* TODO Is this really right? Hooks may be a problem? */
      if (ci->callstatus & (CIST_YPCALL | CIST_YIELDED)) {
        WRITE_VALUE(ci->u.c.ctx, int);
        eris_assert(ci->u.c.k);
        lua_pushcfunction(pi->L, ci->u.c.k);               /* ... thread func */
        persist(pi);                                   /* ... thread func/nil */
        lua_pop(pi->L, 1);                                      /* ... thread */
      }
    }

    /* Write whether there's more to come. */
    WRITE_VALUE(ci->next == thread->ci->next, lu_byte);

    poppath(pi->L);
  }
  poppath(pi->L);

  pushpath(pi->L, ".openupval");
  lua_pushnil(pi->L);                                       /* ... thread nil */
  level = 0;
  for (uv = eris_gco2uv(thread->openupval);
       uv != NULL;
       uv = eris_gco2uv(gch(eris_obj2gco(uv))->next))
  {
    pushpath(pi->L, "[%d]", level);
    WRITE_VALUE(eris_savestack(thread, uv->v), size_t);
    eris_setobj(pi->L, pi->L->top - 1, uv->v);              /* ... thread obj */
    lua_pushlightuserdata(pi->L, uv);                    /* ... thread obj id */
    persist_keyed(pi, LUA_TUPVAL);                          /* ... thread obj */
    poppath(pi->L);
  }
  WRITE_VALUE((size_t)-1, size_t);
  lua_pop(pi->L, 1);                                            /* ... thread */
  poppath(pi->L);
}

static void
u_thread(UnpersistInfo *upi) {                                         /* ... */
  lua_State* thread = lua_newthread(upi->L);                    /* ... thread */
  size_t level;
  StkId o;

  eris_checkstack(upi->L, 3);

  registerobject(upi);

  /* As in p_thread, just to make sure. */
  eris_assert(thread->nny == 1);
  eris_assert(thread->errorJmp == NULL);
  eris_assert(thread->errfunc == 0);
  eris_assert(thread->hook == NULL);

  /* See comment in persist. */
  thread->oldpc = NULL;

  /* Read general information. */
  thread->status = READ_VALUE(lu_byte);
  thread->nCcalls = READ_VALUE(ushort);
  thread->allowhook = READ_VALUE(lu_byte);

  /* Not supported.
  thread->hookmask = READ_VALUE(lu_byte);
  thread->basehookcount = READ_VALUE(int);
  thread->hookcount = READ_VALUE(int); */

  /* Unpersist the stack. Read size first and adjust accordingly. */
  eris_reallocstack(thread, READ_VALUE(int));
  thread->top = thread->stack + READ_VALUE(size_t);
  /* eris_checkstack(thread, level);
  lua_settop(thread, level); */

  /* Read the elements one by one. */
  pushpath(upi->L, ".stack");
  level = 0;
  for (o = thread->stack; o < thread->top; ++o) {
    pushpath(upi->L, "[%d]", level++);
    unpersist(upi);                                         /* ... thread obj */
    eris_setobj(thread, o, upi->L->top - 1);
    lua_pop(upi->L, 1);                                         /* ... thread */
    poppath(upi->L);
  }
  poppath(upi->L);

  /* Read call information (stack frames). */
  pushpath(upi->L, ".callinfo");
  thread->ci = &thread->base_ci;
  level = 0;
  for (;;) {
    pushpath(upi->L, "[%d]", level++);
    thread->ci->func = eris_restorestack(thread, READ_VALUE(size_t));
    thread->ci->top = eris_restorestack(thread, READ_VALUE(size_t));
    thread->ci->nresults = READ_VALUE(short);
    thread->ci->callstatus = READ_VALUE(lu_byte);
    thread->ci->extra = READ_VALUE(ptrdiff_t);

    if (eris_isLua(thread->ci)) {
      LClosure *lcl = eris_ci_func(thread->ci);
      thread->ci->u.l.base = eris_restorestack(thread, READ_VALUE(size_t));
      thread->ci->u.l.savedpc = lcl->p->code + READ_VALUE(size_t);
    }
    else {
      thread->ci->u.c.status = READ_VALUE(lu_byte);

      /* These are only used while a thread is being executed:
      thread->ci->u.c.old_errfunc = READ_VALUE(ptrdiff_t);
      thread->ci->u.c.old_allowhook = READ_VALUE(lu_byte); */

      /* TODO See thread persist function. */
      if (thread->ci->callstatus & (CIST_YPCALL | CIST_YIELDED)) {
        thread->ci->u.c.ctx = READ_VALUE(int);
        unpersist(upi);                                   /* ... thread func? */
        if (lua_iscfunction(upi->L, -1)) {                 /* ... thread func */
          thread->ci->u.c.k = lua_tocfunction(upi->L, -1);
        }
        else {
          luaL_error(upi->L, "bad C continuation function");
          return; /* not reached */
        }
        lua_pop(upi->L, 1);                                     /* ... thread */
      }
      else {
        thread->ci->u.c.ctx = 0;
        thread->ci->u.c.k = NULL;
      }
      thread->ci->u.c.old_errfunc = 0;
      thread->ci->u.c.old_allowhook = 0;
    }
    poppath(upi->L);

    /* Read in value for check for next iteration. */
    if (READ_VALUE(lu_byte)) {
      break;
    }
    else {
      thread->ci = eris_extendCI(thread);
    }
  }
  poppath(upi->L);

  /* Proceed to open upvalues. These upvalues will already exist due to the
   * functions using them having been unpersisted (they'll usually be in the
   * stack of the thread). For this reason we store all previous references to
   * the upvalue in a table that is returned when we try to unpersist an
   * upvalue, so that we can adjust these pointers in here. */
  pushpath(upi->L, ".openupval");
  level = 0;
  for (;;) {
    UpVal *nuv;
    /* Get the position of the upvalue on the stack. As a special value we pass
     * -1 to indicate there are no more upvalues. */
    const size_t offset = READ_VALUE(size_t);
    if (offset == (size_t)-1) {
      break;
    }
    pushpath(upi->L, "[%d]", level);
    unpersist(upi);                                         /* ... thread tbl */
    eris_assert(lua_type(upi->L, -1) == LUA_TTABLE);

    /* Create the open upvalue either way. */
    nuv = eris_findupval(thread, eris_restorestack(thread, offset));

    /* Then check if we need to patch some pointers. */
    lua_rawgeti(upi->L, -1, 2);                   /* ... thread tbl upval/nil */
    if (!lua_isnil(upi->L, -1)) {                     /* ... thread tbl upval */
      int i, n;
      eris_assert(lua_type(upi->L, -1) == LUA_TLIGHTUSERDATA);
      /* Already exists, replace it. To do this we have to patch all the
       * pointers to the already existing one, which we added to the table in
       * u_closure, starting at index 3. */
      lua_pop(upi->L, 1);                                   /* ... thread tbl */
      for (i = 3, n = luaL_len(upi->L, -1); i <= n; ++i) {
        lua_rawgeti(upi->L, -1, i);                  /* ... thread tbl upvalp */
        (*(UpVal**)lua_touserdata(upi->L, -1)) = nuv;
        lua_pop(upi->L, 1);                                 /* ... thread tbl */
      }
    }
    else {                                              /* ... thread tbl nil */
      eris_assert(lua_isnil(upi->L, -1));
      lua_pop(upi->L, 1);                                   /* ... thread tbl */
    }

    /* Store open upvalue in table for future references. */
    lua_pushlightuserdata(upi->L, nuv);               /* ... thread tbl upval */
    lua_rawseti(upi->L, -2, 2);                             /* ... thread tbl */
    lua_pop(upi->L, 1);                                         /* ... thread */
    poppath(upi->L);
  }
  poppath(upi->L);

  eris_assert(lua_type(upi->L, -1) == LUA_TTHREAD);
}

/*
** ============================================================================
** Top-level delegator.
** ============================================================================
*/

static void
persist_typed(PersistInfo *pi, int type) {            /* perms reftbl ... obj */
  eris_ifassert(const int top = lua_gettop(pi->L));
  WRITE_VALUE(type, int);
  switch(type) {
    case LUA_TBOOLEAN:
      p_boolean(pi);
      break;
    case LUA_TLIGHTUSERDATA:
      p_pointer(pi);
      break;
    case LUA_TNUMBER:
      p_number(pi);
      break;
    case LUA_TSTRING:
      p_string(pi);
      break;
    case LUA_TTABLE:
      p_table(pi);
      break;
    case LUA_TFUNCTION:
      p_closure(pi);
      break;
    case LUA_TUSERDATA:
      p_userdata(pi);
      break;
    case LUA_TTHREAD:
      p_thread(pi);
      break;
    case LUA_TPROTO:
      p_proto(pi);
      break;
    case LUA_TUPVAL:
      p_upval(pi);
      break;
    default:
      eris_error(pi->L, "trying to persist unknown type");
  }                                                   /* perms reftbl ... obj */
  eris_assert(top == lua_gettop(pi->L));
}

/* Second-level delegating persist function, used for cases when persisting
 * data that's stored in the reftable with a key that is not the data itself,
 * namely upvalues and protos.
 */
static void
persist_keyed(PersistInfo *pi, int type) {     /* perms reftbl ... obj refkey */
  eris_checkstack(pi->L, 2);

  /* Keep a copy of the key for pushing it to the reftable, if necessary. */
  lua_pushvalue(pi->L, -1);             /* perms reftbl ... obj refkey refkey */

  /* If the object has already been written, write a reference to it. */
  lua_rawget(pi->L, 2);                   /* perms reftbl ... obj refkey ref? */
  if (!lua_isnil(pi->L, -1)) {             /* perms reftbl ... obj refkey ref */
    const int reference = lua_tointeger(pi->L, -1);
    WRITE_VALUE(reference + ERIS_REFERENCE_OFFSET, int);
    lua_pop(pi->L, 2);                                /* perms reftbl ... obj */
    return;
  }                                        /* perms reftbl ... obj refkey nil */
  lua_pop(pi->L, 1);                           /* perms reftbl ... obj refkey */

  /* Copy the refkey for the perms check below. */
  lua_pushvalue(pi->L, -1);             /* perms reftbl ... obj refkey refkey */

  /* Put the value in the reference table. This creates an entry pointing from
   * the object (or its key) to the id the object is referenced by. */
  lua_pushinteger(pi->L, ++(pi->refcount));
                                    /* perms reftbl ... obj refkey refkey ref */
  lua_rawset(pi->L, 2);                        /* perms reftbl ... obj refkey */

  /* At this point, we'll give the permanents table a chance to play. */
  lua_gettable(pi->L, 1);                    /* perms reftbl ... obj permkey? */
  if (!lua_isnil(pi->L, -1)) {                /* perms reftbl ... obj permkey */
    type = lua_type(pi->L, -2);
    /* Prepend permanent "type" so that we know it's a permtable key. This will
     * trigger u_permanent when unpersisting. Also write the original type, so
     * that we can verify what we get in the permtable when unpersisting is of
     * the same kind we had when persisting. */
    WRITE_VALUE(ERIS_PERMANENT, int);
    WRITE_VALUE(type, int);
    persist(pi);                              /* perms reftbl ... obj permkey */
    lua_pop(pi->L, 1);                                /* perms reftbl ... obj */
  }
  else {                                          /* perms reftbl ... obj nil */
    /* No entry in the permtable for this object, persist it directly. */
    lua_pop(pi->L, 1);                                /* perms reftbl ... obj */
    persist_typed(pi, type);                          /* perms reftbl ... obj */
  }                                                   /* perms reftbl ... obj */
}

/* Top-level delegating persist function. */
static void
persist(PersistInfo *pi) {                            /* perms reftbl ... obj */
  /* Grab the object's type. */
  const int type = lua_type(pi->L, -1);

  /* If the object is nil, only write its type. */
  if (type == LUA_TNIL) {
    WRITE_VALUE(type, int);
  }
  /* Write simple values directly, because writing a "reference" would take up
   * just as much space and we can save ourselves work this way.
   */
  else if (type == LUA_TBOOLEAN ||
           type == LUA_TLIGHTUSERDATA ||
           type == LUA_TNUMBER)
  {
    persist_typed(pi, type);                          /* perms reftbl ... obj */
  }
  /* For all non-simple values we keep a record in the reftable, so that we
   * keep references alive across persisting and unpersisting an object. This
   * has the nice side-effect of saving some space. */
  else {
    eris_checkstack(pi->L, 1);
    lua_pushvalue(pi->L, -1);                     /* perms reftbl ... obj obj */
    persist_keyed(pi, type);                          /* perms reftbl ... obj */
  }
}

/** ======================================================================== */

static void
u_permanent(UnpersistInfo *upi) {                         /* perms reftbl ... */
  const int type = READ_VALUE(int);
  /* Reserve reference to avoid the key going first. This registers whatever
   * else is on the stack in the reftable, but that shouldn't really matter. */
  const int reference = registerobject(upi);
  eris_checkstack(upi->L, 1);
  unpersist(upi);                                 /* perms reftbl ... permkey */
  lua_gettable(upi->L, 1);                           /* perms reftbl ... obj? */
  if (lua_isnil(upi->L, -1)) {                        /* perms reftbl ... nil */
    /* Since we may need permanent values to rebuild other structures, namely
     * closures and threads, we cannot allow perms to fail unpersisting. */
    eris_error(upi->L, "bad permanent value (no value)");
  }
  else if (lua_type(upi->L, -1) != type) {             /* perms reftbl ... :( */
    /* For the same reason that we cannot allow nil we must also require the
     * unpersisted value to be of the correct type.
     */
    eris_error(upi->L, "bad permanent value (%s expected, got %s)",
      kTypenames[type], kTypenames[lua_type(upi->L, -1)]);
  }                                                   /* perms reftbl ... obj */
  /* Correct the entry in the reftable. */
  lua_pushvalue(upi->L, -1);                      /* perms reftbl ... obj obj */
  lua_rawseti(upi->L, 2, reference);                  /* perms reftbl ... obj */
}

static void
unpersist(UnpersistInfo *upi) {                           /* perms reftbl ... */
  eris_ifassert(const int top = lua_gettop(upi->L));
  const int typeOrReference = READ_VALUE(int);

  eris_checkstack(upi->L, 1);

  if (typeOrReference > ERIS_REFERENCE_OFFSET) {
    const int reference = typeOrReference - ERIS_REFERENCE_OFFSET;
    lua_rawgeti(upi->L, 2, reference);            /* perms reftbl ud ... obj? */
    if (lua_isnil(upi->L, -1)) {                    /* perms reftbl ud ... :( */
      eris_error(upi->L, "invalid reference #%d", reference);
    }                                              /* perms reftbl ud ... obj */
  }
  else {
    const int type = typeOrReference;
    switch (type) {
      case LUA_TNIL:
        lua_pushnil(upi->L);
        break;
      case LUA_TBOOLEAN:
        u_boolean(upi);
        break;
      case LUA_TLIGHTUSERDATA:
        u_pointer(upi);
        break;
      case LUA_TNUMBER:
        u_number(upi);
        break;
      case LUA_TSTRING:
        u_string(upi);
        break;
      case LUA_TTABLE:
        u_table(upi);
        break;
      case LUA_TFUNCTION:
        u_closure(upi);
        break;
      case LUA_TUSERDATA:
        u_userdata(upi);
        break;
      case LUA_TTHREAD:
        u_thread(upi);
        break;
      case LUA_TPROTO:
        u_proto(upi);
        break;
      case LUA_TUPVAL:
        u_upval(upi);
        break;
      case ERIS_PERMANENT:
        u_permanent(upi);
        break;
      default:
        eris_error(upi->L, "trying to unpersist unknown type %d", type);
    }                                                /* perms reftbl ... obj? */
  }

  eris_assert(top + 1 == lua_gettop(upi->L));
}

/* }======================================================================== */

/*
** {===========================================================================
** Writer and reader implementation for library calls.
** ============================================================================
*/

/* Implementation note: we use the MBuffer struct, but we don't use the built-
 * in reallocation functions since we'll keep our working copy on the stack, to
 * allow for proper collection if we have to throw an error. This is very much
 * what the auxlib does with its buffer functionality. Which we don't use since
 * we cannot guarantee stack balance inbetween calls to luaL_add*.
 */

static int
writer(lua_State *L, const void *p, size_t sz, void *ud) {
                                               /* perms reftbl path? buff ... */
  const char *value = (const char*)p;
  Mbuffer *buff = (Mbuffer*)ud;
  const size_t size = eris_bufflen(buff);
  const size_t capacity = eris_sizebuffer(buff);
  if (capacity - size < sz) {
    size_t newcapacity = capacity * 2; /* overflow checked below */
    if (newcapacity - size < sz) {
      newcapacity = capacity + sz; /* overflow checked below */
    }
    if (newcapacity <= capacity) {
      /* Overflow in capacity, buffer size limit reached. */
      return 1;
    } else {
      char *newbuff = (char*)lua_newuserdata(L, newcapacity * sizeof(char));
                                         /* perms reftbl path? buff ... nbuff */
      memcpy(newbuff, eris_buffer(buff), eris_bufflen(buff));
      lua_replace(L, generatePath ? 4 : 3);   /* perms reftbl path? nbuff ... */
      eris_buffer(buff) = newbuff;
      eris_sizebuffer(buff) = newcapacity;
    }
  }
  memcpy(&eris_buffer(buff)[eris_bufflen(buff)], value, sz);
  eris_bufflen(buff) += sz;
  return 0;
}

/** ======================================================================== */

/* Readonly, interface compatible with MBuffer macros. */
typedef struct RBuffer {
  const char *buffer;
  size_t n;
  size_t buffsize;
} RBuffer;

static const char*
reader(lua_State *L, void *ud, size_t *sz) {
  RBuffer *buff = (RBuffer*)ud;
  (void) L; /* unused */
  if (eris_bufflen(buff) == 0) {
    return NULL;
  }
  *sz = eris_bufflen(buff);
  eris_bufflen(buff) = 0;
  return eris_buffer(buff);
}

/* }======================================================================== */

/*
** {===========================================================================
** Library functions.
** ============================================================================
*/

static void
unchecked_persist(lua_State *L, lua_Writer writer, void *ud) {
                                                       /* perms buff? rootobj */
  PersistInfo pi;
  pi.L = L;
  pi.refcount = 0;
  pi.writer = writer;
  pi.ud = ud;

  lua_newtable(L);                              /* perms buff? rootobj reftbl */
  lua_insert(L, 2);                             /* perms reftbl buff? rootobj */
  if (generatePath) {
    lua_newtable(L);                       /* perms reftbl buff? rootobj path */
    lua_insert(L, 3);                      /* perms reftbl path buff? rootobj */
    pushpath(L, "root");
  }

  /* Populate perms table with Lua internals. */
  lua_pushvalue(L, 1);
  populateperms(L, 0);
  lua_pop(L, 1);

  persist(&pi);                           /* perms reftbl path? buff? rootobj */

  if (generatePath) {                      /* perms reftbl path buff? rootobj */
    lua_remove(L, 3);                           /* perms reftbl buff? rootobj */
  }                                             /* perms reftbl buff? rootobj */
  lua_remove(L, 2);                                    /* perms buff? rootobj */
}

static void
unchecked_unpersist(lua_State *L, lua_Reader reader, void *ud) {/* perms str? */
  UnpersistInfo upi;
  upi.L = L;
  upi.refcount = 0;
  eris_init(L, &upi.zio, reader, ud);

  lua_newtable(L);                                       /* perms str? reftbl */
  lua_insert(L, 2);                                      /* perms reftbl str? */
  if (generatePath) {
    lua_newtable(L);                                /* perms reftbl str? path */
    lua_insert(L, 3);                               /* perms reftbl path str? */
    pushpath(L, "root");
  }

  /* Populate perms table with Lua internals. */
  lua_pushvalue(L, 1);
  populateperms(L, 1);
  lua_pop(L, 1);

  lua_gc(L, LUA_GCSTOP, 0);

  unpersist(&upi);                         /* perms reftbl path? str? rootobj */
  if (generatePath) {                       /* perms reftbl path str? rootobj */
    lua_remove(L, 3);                            /* perms reftbl str? rootobj */
  }                                              /* perms reftbl str? rootobj */
  lua_remove(L, 2);                                     /* perms str? rootobj */

  lua_gc(L, LUA_GCRESTART, 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
}

/** ======================================================================== */

static int
l_persist(lua_State *L) {                             /* perms? rootobj? ...? */
  Mbuffer buff;

  /* If we only have one object we assume it is the root object and that there
   * is no perms table, so we create an empty one for internal use. */
  if (lua_gettop(L) == 1) {                                        /* rootobj */
    eris_checkstack(L, 1);
    lua_newtable(L);                                         /* rootobj perms */
    lua_insert(L, 1);                                        /* perms rootobj */
  }
  else {
    luaL_checktype(L, 1, LUA_TTABLE);                  /* perms rootobj? ...? */
    luaL_checkany(L, 2);                                /* perms rootobj ...? */
    lua_settop(L, 2);                                        /* perms rootobj */
  }
  eris_checkstack(L, 1);
  lua_pushnil(L);                                       /* perms rootobj buff */
  lua_insert(L, 2);                                     /* perms buff rootobj */

  eris_initbuffer(L, &buff);
  eris_bufflen(&buff) = 0; /* Not initialized by initbuffer... */

  unchecked_persist(L, writer, &buff);                  /* perms buff rootobj */

  /* Copy the buffer as the result string before removing it, to avoid the data
   * being garbage collected. */
  lua_pushlstring(L, eris_buffer(&buff), eris_bufflen(&buff));
                                                    /* perms buff rootobj str */

  return 1;
}

static int
l_unpersist(lua_State *L) {                               /* perms? str? ...? */
  RBuffer buff;

  /* If we only have one object we assume it is the root object and that there
   * is no perms table, so we create an empty one for internal use. */
  if (lua_gettop(L) == 1) {                                           /* str? */
    eris_checkstack(L, 1);
    lua_newtable(L);                                            /* str? perms */
    lua_insert(L, 1);                                           /* perms str? */
  }
  else {
    luaL_checktype(L, 1, LUA_TTABLE);                      /* perms str? ...? */
  }
  eris_buffer(&buff) = luaL_checklstring(L, 2, &eris_bufflen(&buff));
  eris_sizebuffer(&buff) = eris_bufflen(&buff);             /* perms str ...? */
  lua_settop(L, 2);                                              /* perms str */

  unchecked_unpersist(L, reader, &buff);                 /* perms str rootobj */

  return 1;
}

/** ======================================================================== */

static luaL_Reg erislib[] = {
  { "persist", l_persist },
  { "unpersist", l_unpersist },
  { NULL, NULL }
};

LUA_API int luaopen_eris(lua_State *L) {
  luaL_newlib(L, erislib);
  return 1;
}

/* }======================================================================== */

/*
** {===========================================================================
** Public API functions.
** ============================================================================
*/

void
eris_dump(lua_State *L, lua_Writer writer, void *ud) {     /* perms? rootobj? */
  if (lua_gettop(L) > 2) {
    luaL_error(L, "too many arguments");
  }
  luaL_checktype(L, 1, LUA_TTABLE);                         /* perms rootobj? */
  luaL_checkany(L, 2);                                       /* perms rootobj */
  unchecked_persist(L, writer, ud);                          /* perms rootobj */
}

void
eris_undump(lua_State *L, lua_Reader reader, void *ud) {            /* perms? */
  if (lua_gettop(L) > 1) {
    luaL_error(L, "too many arguments");
  }
  luaL_checktype(L, 1, LUA_TTABLE);                                  /* perms */
  unchecked_unpersist(L, reader, ud);                        /* perms rootobj */
}

/** ======================================================================== */

void
eris_persist(lua_State* L, int perms, int value) {                    /* ...? */
  eris_checkstack(L, 3);
  lua_pushcfunction(L, l_persist);                           /* ... e_persist */
  lua_pushvalue(L, perms);                             /* ... e_persist perms */
  lua_pushvalue(L, value);                     /* ... e_persist perms rootobj */
  lua_call(L, 2, 1);                                               /* ... str */
}

void
eris_unpersist(lua_State* L, int perms, int value) {                   /* ... */
  eris_checkstack(L, 3);
  lua_pushcfunction(L, l_unpersist);                       /* ... e_unpersist */
  lua_pushvalue(L, perms);                           /* ... e_unpersist perms */
  lua_pushvalue(L, value);                       /* ... e_unpersist perms str */
  lua_call(L, 2, 1);                                           /* ... rootobj */
}

/* }======================================================================== */

