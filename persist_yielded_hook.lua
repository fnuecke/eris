local cr = coroutine.create(function()
  for i = 1, 10 do
    print('cr:', i)
  end
end)

debug.sethook(cr, function() return true end, 'y', 20)

print('A')
coroutine.resume(cr)
print('B')

local t = eris.persist({[_ENV] = 1, [print] = 2}, cr)

print('C')
coroutine.resume(cr)
print('D')

cr = eris.unpersist({_ENV, print}, t)

print('E')
coroutine.resume(cr)
print('F')

