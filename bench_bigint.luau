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
    local a = bigint.fromstring("1000")
    local b = bigint.fromstring("2000")
    local sum = bigint.fromstring("0")
    local t0 = os.clock()
    for i = 1, 10000000 do
        sum = a + b
    end
    print(string.format("Dynamic BigInt: %.4f seconds (result: %s)", os.clock() - t0, tostring(sum)))
end

local function bench_u32()
    local a = bigint.u32(bigint.fromstring("1000"))
    local b = bigint.u32(bigint.fromstring("2000"))
    local sum = bigint.u32(bigint.fromstring("0"))
    local t0 = os.clock()
    for i = 1, 10000000 do
        sum = a + b
    end
    print(string.format("Wrapping u32: %.4f seconds (result: %s)", os.clock() - t0, tostring(sum)))
end

bench_number()
bench_dynamic()
bench_u32()
