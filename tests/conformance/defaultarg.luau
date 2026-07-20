
-- This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
print("testing function argument default values")

local function test_local_access()
    local CONSTANT_VALUE = 1
    -- Ensure we're accessing upvalues correctly
    function foo(x = CONSTANT_VALUE) end
end
test_local_access()

local function test_nil_omit()
    -- It should be possible to specify y without needing to specify x
    function foo(x = 1, y = 2)
        return x, y
    end

    local x, y = foo(nil, 3)
    assert(x == 1)
    assert(y == 3)
end
test_nil_omit()

local function test_not_singleton()
    -- The default argument should be evaluated every time the function is called
    function foo(x = {a = 0})
        x.a += 1
        return x.a
    end
    assert(foo() == foo())
end
test_not_singleton()

local function test_complex_self()
    local Foo = {}

    function Foo.inc(self = { count = 0, inc = Foo.inc }, val = 1)
      self.count += val
      return self
    end

    local foo = { count = 0, inc = Foo.inc }
    assert(foo:inc().count == 1)
    assert(foo:inc().count == 2)
    assert(foo:inc(2).count == 4)

    assert(Foo.inc().count == 1)
    assert(Foo.inc().count == 1)
    assert(Foo.inc(foo).count == 5)
    assert(Foo.inc(foo, 2).count == 7)
end
test_complex_self()

local function test_metatable()
    local Foo = {}

    function Foo.inc(
        self = setmetatable({
                count = 0,
            }, {
                __index = Foo,
            }
        ),
        val = 1
    )
        self.count += val
        return self
    end

    local foo = Foo.inc()
    assert(foo.count == 1)
    assert(foo:inc().count == 2)
    assert(foo:inc(2).count == 4)
end
test_metatable()

local function test_index_default()
    local mt = {}
    function mt.__index(self, key = "count")
        return rawget(self, key)
    end

    local foo = setmetatable({ count = 5 }, mt)
    -- This "shouldn't" work but our default argument made it work
    assert(foo[nil] == 5)
end
test_index_default()

return "OK"
