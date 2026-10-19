import { instantiate } from "../wabt-wrapper.js";
import * as assert from "../assert.js";

// rem_s with INT_MIN as dividend and a divisor that is -1 only at runtime must
// return 0 (the spec defines it; only div_s traps on that overflow). Constant -1
// divisors are folded by the compilers, so the divisor comes in as a parameter.
// Both the register/register form and the constant-dividend form are covered,
// and the loop runs long enough for the functions to tier up.

let wat = `
(module
    (func (export "remS32") (param i32 i32) (result i32)
        local.get 0
        local.get 1
        i32.rem_s
    )
    (func (export "remS64") (param i64 i64) (result i64)
        local.get 0
        local.get 1
        i64.rem_s
    )
    (func (export "remS32ConstMin") (param i32) (result i32)
        i32.const -2147483648
        local.get 0
        i32.rem_s
    )
    (func (export "remS64ConstMin") (param i64) (result i64)
        i64.const -9223372036854775808
        local.get 0
        i64.rem_s
    )
)
`;

async function test() {
    const instance = await instantiate(wat, {}, {});
    const { remS32, remS64, remS32ConstMin, remS64ConstMin } = instance.exports;

    const I32_MIN = -2147483648;
    const I64_MIN = -9223372036854775808n;

    for (let i = 0; i < wasmTestLoopCount; ++i) {
        assert.eq(remS32(I32_MIN, -1), 0);
        assert.eq(remS32(I32_MIN, 3), -2);
        assert.eq(remS32(7, -1), 0);
        assert.eq(remS32(-7, 2), -1);
        assert.eq(remS32(2147483647, -1), 0);

        assert.eq(remS64(I64_MIN, -1n), 0n);
        assert.eq(remS64(I64_MIN, 3n), -2n);
        assert.eq(remS64(7n, -1n), 0n);
        assert.eq(remS64(-7n, 2n), -1n);

        assert.eq(remS32ConstMin(-1), 0);
        assert.eq(remS32ConstMin(3), -2);
        assert.eq(remS64ConstMin(-1n), 0n);
        assert.eq(remS64ConstMin(3n), -2n);
    }

    assert.throws(() => remS32(1, 0), WebAssembly.RuntimeError, "Division by zero");
    assert.throws(() => remS64(1n, 0n), WebAssembly.RuntimeError, "Division by zero");
}

await assert.asyncTest(test());
