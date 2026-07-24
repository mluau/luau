local function bench_number()
    local a = 1000
    local b = 2000
    local sum = 0
    local t0 = os.clock()
    for i = 1, 10000000 do
        sum = a + b
    end
    print(string.format("Number: %.4f seconds (result: %s)", os.clock() - t0, tostring(sum)))
end

local function bench_dynamic()
    local a = integer.fromstring("1000")
    local b = integer.fromstring("2000")
    local sum = integer.fromstring("0")
    local t0 = os.clock()
    for i = 1, 10000000 do
        sum = a + b
    end
    print(string.format("Dynamic Integer: %.4f seconds (result: %s)", os.clock() - t0, tostring(sum)))
end

local function bench_u32()
    local a = integer.u32(integer.fromstring("1000"))
    local b = integer.u32(integer.fromstring("2000"))
    local sum = integer.u32(integer.fromstring("0"))
    local t0 = os.clock()
    for i = 1, 10000000 do
        sum = a + b
    end
    print(string.format("Wrapping u32: %.4f seconds (result: %s)", os.clock() - t0, tostring(sum)))
end

bench_number()
bench_dynamic()
bench_u32()
