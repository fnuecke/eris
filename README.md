Eris - Heavy Duty Persistence for Lua
=====================================

First things first: this is essentially a rewrite of [Pluto][], targeting Lua 5.2. If you need something like this for Lua 5.1 I strongly suggest you have a look at that. In fact, have a look at it anyway, because the documentation isn't half bad, and a lot of it applies to Eris, too.

Although my implementation is strongly influenced by Pluto, in my humble opinion there are still sufficient changes in the architecture that a new name is appropriate. So, to stick with the theme, I named it Eris. Note that all those changes are internal. The API is almost left untouched to make a transition easier.

Eris can serialize almost anything you can have in a Lua VM and can unserialize it again at a later point, even in a different VM. This in particular includes yielded coroutines, which is very handy for saving long running, multi-session scripting systems, for example in games: just persist the state into the save file and unpersist it upon load, and the scripts won't even know the game was quit inbetween.

Eris currently requires Lua 5.2.2. It may build with earlier 5.2.x versions, but I have not tried it.

At this point, Eris should be considered as a beta-stage product. I have tested it somewhat and the testcases that came with Pluto - extended with some Lua 5.2 specific ones, such as yielded `pcall`s - run through successfully. This does not mean it is necessarily stable, though. You have been warned.

Installation
============

Think of Eris as a custom "distribution" of Lua. Compile and install Eris like you would Lua. You will get one shared library that will contain both Lua and Eris. The version string has been adjusted to reflect this: `Lua+Eris 5.2.2`.

Usage
=====

C API
-----

Like Pluto, Eris offers two functions to persist to or read from an arbitrary source. Eris uses the Lua typedefs `lua_Writer` and `lua_Reader` for this purpose, the replacements of `lua_Chunkwriter` and `lua_Chunkreader` used by Pluto.

* `void eris_dump(lua_State *L, lua_Writer writer, void *ud);` `[-0, +0, e]`  
  This provides an interface to Eris' persist functionality for writing in an arbitrary way, using a writer. When called, the stack in `L` must look like this:
  1. `perms:table`
  2. `value:any`  

  That is, `perms` must be a table at stack position 1. This table is used as the permanent object table. `value` is the value that should be persisted. It can be any persistable Lua value. 'writer' is the writer function used to store all data, `ud` is passed to the writer function whenever it is called.

  This function is equivalent to `pluto_persist`.

* `void eris_undump(lua_State *L, lua_Reader reader, void *ud);` `[-0, +1, e]`  
  This provides an interface to Eris' unpersist functionality for reading in an arbitrary way, using a reader. When called, the stack in `L` must look like this:
  1. `perms:table`  

  That is, `perms` must be a table at stack position 1. This table is used as the permanent objcet table. This must hold the inverse mapping present in the permanent object table used when persisting. `reader` is the reader function used to read all data, `ud` is passed to the reader function whenever it is called. The result of the operation will be pushed onto the stack.

  This function is equivalent to `pluto_unpersist`. The function will only check the stack's top. Like Pluto, Eris uses Lua's own ZIO to handle buffered reading. Note that unlike with Pluto, the value can in fact be `nil`.

In addition to these, Eris also offers two more convenient functions, if you simply wish to persist an object to a string. These behave like the functions exposed to Lua.

* `void eris_persist(lua_State *L, int perms, int value);` `[-0, +1, e]`  
  It expects the permanent object table at the specified index `perms` and the value to persist at the specified index `value`. It will push the resulting binary string onto the stack on success.

* `void eris_unpersist(lua_State *L, int perms, int value);` `[-0, +1, e]`  
  It expects the permanent object table at the specified index `perms` and the binary string containing persisted data at the specified index `value`. It will push the resulting value onto the stack on success.

The subtle name change from Pluto was done because Lua's own dump/undump works with a writer/reader, so it felt more consistent this way.

Lua
---

You can either load Eris as a table onto the Lua stack via `luaopen_eris()` or just call `luaL_openlibs()` which will open it in addition to all other libraries and register the global table `eris`. As with Pluto, there are two functions in this table:

* `string eris.persist([perms,] value)`  
  This persists the provided value and returns the resulting data as a binary string. Note that passing the permanent object table is optional. If only one argument is given Eris assumes it's the object to persist, and the permanent object table is empty. If given, `perms` must be a table. `value` can be any type.

* `any eris.unpersist([perms,] value)`  
  This unpersists the provided binary string that resulted from an earlier call to `eris.persist()` and return the unpersisted value. Note that passing the permanent object table is optional. If only one argument is given Eris assumes it's the data representing a persisted object, and the permanent object table is empty. If given, `perms` must be a table. `value` must be a string.

Concepts
========

Persistence
-----------

Eris will persist most objects out of the box. This includes basic value types (nil, boolean, light userdata (as the literal pointer value), number) as well as strings, tables, closures and threads. For tables and userdata, metatables are also persisted. Tables, functions and threads are persisted recursively, i.e. each value referenced in them (key/value, upvalue, stack) will in turn be persisted.

C closures are only supported if the underlying C function is in the permanent object table. Userdata is only supported if the special persistence metafield is present (see below).

Like Pluto, Eris will store objects only once and reference this first occasion whenever the object should be persisted again. This ensures that references are kept across persisting an object, and has the nice side effect of keeping the size of the output small.

Permanent Objects
-----------------

Whenever Eris tries to persist any non-basic value (i.e. *not* nil, boolean, light userdata or number) it will check if the value it is about to persist is a key in the table of permanent objects. If it is, Eris will write the associated value instead (the "permanent key"). When unpersisting, the opposite process takes place: for each value persisted this way Eris will look for the permanent key in the permanent object table, and use the value associated with that key.

For example, this allows persisting values that reference API functions, i.e. C functions provided by the native host. These cannot be persisted literally, because what it boils down to is that those are function pointers, and these may change over multiple program runs or between machines.

Note that Eris requires the value type to be the same before persisting and after unpersisting. That is, if the value replaced with a key from the permanent object table was a string, it must be a string that is stored permanent object table used for unpersisting at that key:
```lua
> eris.unpersist({[1] = "asd"}, eris.persist({["asd"] = 1}, "asd")) -- OK
> eris.unpersist({[1] = true}, eris.persist({["asd"] = 1}, "asd"))
stdin:1: bad permanent value (string expected, got boolean)
```

The permanent object table must not contain two identical values, i.e. two different objects must not be persisted with the same permanent key. This would lead to errors when unpersisting if the original types mismatch, and weird behavior otherwise. You must ensure this yourself, Eris does no checks in this regard.

Permanent keys starting with `__eris.` are reserved for internal use. For now they are used for functions internal to Lua's libraries, namely the resume functions for yieldable C functions such as `pcall`.

Special Persistence
-------------------

Another concept carried over from Pluto is "special persistence". Tables and userdata can be annotated with a metatable field (named `__persist` per default). This field can be one of three types: `nil` for default behavior, `boolean` or `function`:

* `true` marks the object for literal persistence. This is the default for tables. Trying to literally persist userdata without this will result in an error. If set, however, the userdata's memory block will be written as-is, and read back as-is.
* `false` marks the object as "forbidden". When such an object is encountered an error will be thrown.
* `function` is very special indeed. If present, this function will called when the object should be persisted. It must return a closure, which is persisted in place of the original object. The returned closure is then run when unpersisting, and is expected to return an object of the object's original type (i.e. table or userdata). Per default, the function stored in the metatable will receive one parameter, the original object. You can configure Eris to also pass along the used writer and userdata associated with it (see `eris_dump()`).

  Here is an example, originally from the Pluto documentation:
  ```lua
  vec = { x = 2, y = 1, z = 4 }
  setmetatable(vec, { __persist = function(oldtbl)
    local x = oldtbl.x
    local y = oldtbl.y
    local z = oldtbl.z
    local mt = getmetatable(oldtbl)
    return function()
      newtbl = {}
      newtbl.x = x
      newtbl.y = y
      newtbl.z = z
      setmetatable(newtbl, mt)
      return newtbl
    end
  end })
  ```

  Also from the Pluto documentation:
  > It is important that the fixup closure returned not reference the original table directly, as that table would again be persisted as an upvalue, leading to an infinite loop. Also note that the object's metatable is *not* automatically persisted; it is necessary for the fixup closure to reset it, if it wants.

Limitations
===========

* C functions cannot be persisted. Put them in the permanent object table.
  - C closures can only be persisted if the underlying C function is in the permanent object table.
* Hook information is not persisted. You have to re-install hooks for persisted threads after unpersisting them.
* Metatables of basic types are not persisted. A workaround may be to add a dummy table with a `__persist` callback, which in turn stores these metatables, reinstalls them and returns a dummy table upon unpersisting.
* Neither the thread running Eris nor its parents can be persisted. More generally: no *running* thread can be persisted, only suspended ones - either because they were not started yet, or because they yielded.
* Threads that yielded out of a hook function cannot be persisted. I have not even tested this, since this seems to only be possible for certain hooks (line and count) and when the hook function yields from C itself.
* When the loaded data contains functions, this is like loading binary code, and may therefore be potentially unsafe. You should only unpersist trusted data.

Core library
============

You will notice that I decided to bundle this with Lua 5.2.2 directly, instead of as a standalone library. This is in part due to how deeply Eris has to interact with Lua. It has to manually create internal data structures and populate them. Later versions of Pluto worked around this by extracting the used internal functions and only bundling those with the library. I decided against this for now, since with Lua 5.2 there's another thing that I need access to: the internally used C resume functions, used for yieldable C calls in the libraries (e.g. `pcall`). I had to patch the library sources by adding a function that pushed these C functions to the table with permanent values. These patches are very unintrusive: they just add a couple of lines to the end of the relevant library files. This means it is possible to persist a yielded `pcall` out of the box.

Testing
=======

Eris uses the same test suite Pluto uses, extended with a couple of Lua 5.2 specific tests, such as persisting yielded `pcall`s. For now there is no way to automatically build these (sorry).

Differences to Pluto
====================

Quite obviously most design choices were taken from Pluto, partially to make it easier to migrate for people already familiar with Pluto, largely because they are pretty good as they are. There are some minute differences however.

* The resulting persisted data, while using the same basic idea, is structured slightly differently.
* On the C side, Eris provides two new functions, `eris_persist` and `eris_unpersist` which work on with the stack, so you don't have to write your own `lua_Writer`/`lua_Reader` implementation.
* Better error reporting. When debugging, I recommend enabling `generatePath` (directly in the code until I decide how to best pass these options along). This will result in all error messages containing a "path" that specifies where in the object the error occurred. For example:

  ```lua
  > eris.persist({
  >> good = true,
  >> [false] = setmetatable({}, {
  >>   __index = setmetatable({}, {__persist = false})
  >> })})
  stdin:1: attempt to persist forbidden table (root[false]@metatable.__index)
  ```
  This is disabled per default due to the additional processing and memory overhead.


[Pluto]: https://github.com/hoelzro/pluto
