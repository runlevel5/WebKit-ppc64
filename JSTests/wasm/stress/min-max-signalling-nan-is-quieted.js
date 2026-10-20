import { instantiate } from "../wabt-wrapper.js";
import * as assert from "../assert.js";

// f32/f64 min and max with a signalling NaN operand must return an arithmetic
// (quiet) NaN. Some ISAs' JS-style min/max instructions (POWER9's xsmaxjdp /
// xsminjdp) pass a signalling NaN through unchanged, so they are not usable
// for wasm. The NaNs are built from integer bits so they reach the operation
// unquieted, and the loop runs long enough for the functions to tier up.

let wat = `
(module
    (func (export "minF64") (param i64 f64) (result i64)
        (i64.reinterpret_f64 (f64.min (f64.reinterpret_i64 (local.get 0)) (local.get 1))))
    (func (export "maxF64") (param i64 f64) (result i64)
        (i64.reinterpret_f64 (f64.max (f64.reinterpret_i64 (local.get 0)) (local.get 1))))
    (func (export "minF64Swapped") (param i64 f64) (result i64)
        (i64.reinterpret_f64 (f64.min (local.get 1) (f64.reinterpret_i64 (local.get 0)))))
    (func (export "maxF64Swapped") (param i64 f64) (result i64)
        (i64.reinterpret_f64 (f64.max (local.get 1) (f64.reinterpret_i64 (local.get 0)))))
    (func (export "minF32") (param i32 f32) (result i32)
        (i32.reinterpret_f32 (f32.min (f32.reinterpret_i32 (local.get 0)) (local.get 1))))
    (func (export "maxF32") (param i32 f32) (result i32)
        (i32.reinterpret_f32 (f32.max (f32.reinterpret_i32 (local.get 0)) (local.get 1))))
    (func (export "minF32Swapped") (param i32 f32) (result i32)
        (i32.reinterpret_f32 (f32.min (local.get 1) (f32.reinterpret_i32 (local.get 0)))))
    (func (export "maxF32Swapped") (param i32 f32) (result i32)
        (i32.reinterpret_f32 (f32.max (local.get 1) (f32.reinterpret_i32 (local.get 0)))))
)
`;

function isQuietNaN64(bits)
{
    const exponentMask = 0x7ff0000000000000n;
    const quietBit = 0x0008000000000000n;
    return (bits & exponentMask) === exponentMask && (bits & quietBit) !== 0n;
}

function isQuietNaN32(bits)
{
    const exponentMask = 0x7f800000;
    const quietBit = 0x00400000;
    return (bits & exponentMask) === exponentMask && (bits & quietBit) !== 0;
}

async function test() {
    const instance = await instantiate(wat, {}, {});
    const { minF64, maxF64, minF64Swapped, maxF64Swapped, minF32, maxF32, minF32Swapped, maxF32Swapped } = instance.exports;

    const sNaN64 = [0x7ff0000000000001n, -0x000fffffffffffffn]; // the second is 0xfff0000000000001
    const sNaN32 = [0x7f800001, 0xff800001 | 0];
    const others = [1.5, -0, 0, -Infinity, NaN];

    for (let i = 0; i < wasmTestLoopCount; ++i) {
        for (const nan of sNaN64) {
            for (const other of others) {
                assert.truthy(isQuietNaN64(minF64(nan, other)));
                assert.truthy(isQuietNaN64(maxF64(nan, other)));
                assert.truthy(isQuietNaN64(minF64Swapped(nan, other)));
                assert.truthy(isQuietNaN64(maxF64Swapped(nan, other)));
            }
        }
        for (const nan of sNaN32) {
            for (const other of others) {
                assert.truthy(isQuietNaN32(minF32(nan, other)));
                assert.truthy(isQuietNaN32(maxF32(nan, other)));
                assert.truthy(isQuietNaN32(minF32Swapped(nan, other)));
                assert.truthy(isQuietNaN32(maxF32Swapped(nan, other)));
            }
        }
    }
}

await assert.asyncTest(test());
