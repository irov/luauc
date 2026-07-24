-- This file is part of the luauc C port of the Luau programming language
-- and is licensed under the MIT License; see LICENSE.txt for details.
local function fib(n)
    if n < 2 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end

assert(fib(10) == 55)

local values = {answer = 40}
setmetatable(values, {
    __index = function(_, key)
        return key == "extra" and 2 or nil
    end,
})
assert(values.answer + values.extra == 42)

local thread = coroutine.create(function(value)
    local resumed = coroutine.yield(value + 1)
    return resumed + 1
end)

local ok, value = coroutine.resume(thread, 40)
assert(ok and value == 41)
ok, value = coroutine.resume(thread, 41)
assert(ok and value == 42)

local data = buffer.create(8)
buffer.writei32(data, 0, -42)
buffer.writef32(data, 4, 1.5)
assert(buffer.readi32(data, 0) == -42)
assert(buffer.readf32(data, 4) == 1.5)
