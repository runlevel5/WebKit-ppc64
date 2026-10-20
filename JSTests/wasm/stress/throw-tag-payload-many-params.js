import { instantiate } from "../wabt-wrapper.js";
import * as assert from "../assert.js";

// The JIT tiers spill a thrown tag's payload at sp and pass that address to the
// C++ throw operation. Every payload slot must survive the call, including the
// ones a C callee could treat as its caller's linkage area.

let wat = `
(module
    (tag $t (param i32 i64 f32 f64 i32 i64))

    (func $thrower (param $x i32)
        local.get $x
        i64.const 0x123456789a
        f32.const 1.5
        f64.const 2.25
        i32.const 77
        i64.const -5
        throw $t
    )

    (func (export "test") (param $x i32) (result i32)
        (local $a i32) (local $b i64) (local $c f32) (local $d f64) (local $e i32) (local $f i64)
        try
            local.get $x
            call $thrower
        catch $t
            local.set $f
            local.set $e
            local.set $d
            local.set $c
            local.set $b
            local.set $a
        end
        (i32.or
            (i32.or
                (i32.or
                    (i32.ne (local.get $a) (local.get $x))
                    (i32.shl (i64.ne (local.get $b) (i64.const 0x123456789a)) (i32.const 1)))
                (i32.or
                    (i32.shl (f32.ne (local.get $c) (f32.const 1.5)) (i32.const 2))
                    (i32.shl (f64.ne (local.get $d) (f64.const 2.25)) (i32.const 3))))
            (i32.or
                (i32.shl (i32.ne (local.get $e) (i32.const 77)) (i32.const 4))
                (i32.shl (i64.ne (local.get $f) (i64.const -5)) (i32.const 5))))
    )
)
`;

async function test() {
    const instance = await instantiate(wat, {}, { exceptions: true });
    const { test } = instance.exports;
    for (let i = 0; i < wasmTestLoopCount; ++i)
        assert.eq(test(i), 0);
}

await assert.asyncTest(test());
