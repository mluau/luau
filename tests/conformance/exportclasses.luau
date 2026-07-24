--!nocheck

-- `export` statements force the module's implicit return to be the (frozen) export table, which
-- conflicts with an explicit top-level `return`. So the exported class under test lives in its own
-- chunk, loaded and run via loadstring, while this outer chunk stays export-free and can `return 'OK'`.
-- Since the inner chunk can't `return` its result either (same export/return conflict), it stashes
-- the result in a global instead.
local source = [[
    export class Cat
        public name: string
        public age: number
        function new(name: string)
            return Cat {
                name = name,
                age = 0
            }
        end
    end

    catTestResult = Cat.new("Cat will disappear")
]]

local chunk, compileErr = loadstring(source)
assert(chunk, `failed to compile export class chunk: {tostring(compileErr)}`)

local ok, err = pcall(chunk)

assert(ok, `expected Cat.new (called from within Cat's own class scope) to succeed, got error: {tostring(err)}`)

local result = catTestResult
assert(typeof(result) == "object", `expected typeof(result) == "object", got {typeof(result)}`)

print("ok: exported class is not nil within its own scope")

return 'OK'
