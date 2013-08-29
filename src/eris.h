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

/* lua.h must be included before this file */

/*
** ==================================================================
** API
** ==================================================================
*/

/**
 * This provides an interface to Eris' persist functionality for writing in
 * an arbitrary way, using a writer.
 *
 * When called, the stack in 'L' must look like this:
 * 1: perms[table], 2: value[any]
 *
 * 'writer' is the writer function used to store all data, 'ud' is passed to
 * the writer function whenever it is called.
 *
 * [-0, +0, e]
 */
LUA_API void eris_dump(lua_State* L, lua_Writer writer, void* ud);

/**
 * This provides an interface to Eris' unpersist functionality for reading
 * in an arbitrary way, using a reader.
 *
 * When called, the stack in 'L' must look like this:
 * 1: perms[table]
 *
 * 'reader' is the reader function used to read all data, 'ud' is passed to
 * the reader function whenever it is called.
 *
 * The result of the operation will be pushed onto the stack.
 *
 * [-0, +1, e]
 */
LUA_API void eris_undump(lua_State* L, lua_Reader reader, void* ud);

/**
 * This is a stack-based alternative to eris_persistto.
 *
 * It expects the perms table at the specified index 'perms' and the value to
 * persist at the specified index 'value'. It will push the resulting string
 * onto the stack on success.
 *
 * [-0, +1, e]
 */
LUA_API void eris_persist(lua_State* L, int perms, int value);

/**
 * This is a stack-based alternative to eris_unpersistfrom.
 *
 * It expects the perms table at the specified index 'perms' and the string
 * containing persisted data at the specified index 'value'. It will push the
 * resulting value onto the stack on success.
 *
 * [-0, +1, e]
 */
LUA_API void eris_unpersist(lua_State* L, int perms, int value);

/*
** ==================================================================
** Library installer
** ==================================================================
*/

/**
 * This pushes a table with the two functions 'persist' and 'unpersist':
 *   string persist(perms, value)
 *     Where 'perms' is a table with "permanent" objects and 'value' is the
 *     value that should be persisted. Returns the string with persisted data.
 *
 *   table unpersist(perms, value)
 *     Where 'perms' is a table with the inverse mapping as used when
 *     persisting the data via persist() and 'value' is the string with the
 *     persisted data returned by persist(). Returns the unpersisted value.
 */
LUA_API int luaopen_eris(lua_State* L);