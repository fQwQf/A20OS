assert(_VERSION == 'Lua 5.5')
local values = {}
for i = 1, 10000 do values[i] = i end
local sum = 0
for _, value in ipairs(values) do sum = sum + value end
assert(sum == 50005000)
local co = coroutine.create(function()
  for i = 1, 50 do coroutine.yield(i * i) end
end)
for i = 1, 50 do
  local ok, value = coroutine.resume(co)
  assert(ok and value == i * i)
end
assert(coroutine.resume(co))
local path = '/mlibc-lua-data'
local f = assert(io.open(path, 'w+b'))
assert(f:write(string.rep('native\0', 4096)))
assert(f:seek('set', 7) == 7)
assert(f:read(7) == 'native\0')
assert(f:seek('end') == 7 * 4096)
assert(f:close())
assert(os.rename(path, path .. '-renamed'))
f = assert(io.open(path .. '-renamed', 'rb'))
assert(#f:read('a') == 7 * 4096)
f:close()
assert(os.remove(path .. '-renamed'))
local dumped = string.dump(function(a, b) return a + b end)
assert(assert(load(dumped))(19, 23) == 42)
assert(string.format('%d:%.2f', 42, 3.5) == '42:3.50')
assert(string.match('Native ABI 2026', 'ABI (%d+)') == '2026')
collectgarbage('collect')
print('LUA_NATIVE: PASS tables coroutine binary-io seek rename bytecode formatting GC')
