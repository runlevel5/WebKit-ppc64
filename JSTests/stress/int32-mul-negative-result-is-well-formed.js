//@ runDefault
//@ runNoJIT
// A negative int32 product must be boxed as a well-formed int32 JSValue in every
// tier, including the interpreter: strict equality, Array.prototype.indexOf,
// Map/Set lookup and switch all compare the boxed bits, not the unboxed value.

function mul(a, b) { return a * b; }
noInline(mul);

function shouldBeTrue(condition, message) {
    if (!condition)
        throw new Error(message);
}

const map = new Map([[-6, "six"], [-2147483646, "big"]]);
const set = new Set([-6, -2147483646]);
function dispatch(v) {
    switch (v) {
    case -6: return 1;
    case -2147483646: return 2;
    default: return 0;
    }
}

for (let i = 0; i < 1000; ++i) {
    const a = mul(-2, 3);
    const b = mul(2, -3);
    const c = mul(-2147483646, 1);
    const d = mul(1073741823, -2);
    shouldBeTrue(a === -6, "mul(-2, 3) === -6");
    shouldBeTrue(b === -6, "mul(2, -3) === -6");
    shouldBeTrue(a === b, "mul(-2, 3) === mul(2, -3)");
    shouldBeTrue([a].indexOf(-6) === 0, "indexOf on a negative product");
    shouldBeTrue(map.get(a) === "six", "Map.get with a negative product");
    shouldBeTrue(set.has(b), "Set.has with a negative product");
    shouldBeTrue(dispatch(a) === 1, "switch on a negative product");
    shouldBeTrue(c === -2147483646 && map.get(c) === "big", "large negative product");
    shouldBeTrue(dispatch(d) === 2, "switch on a large negative product");
    shouldBeTrue(mul(-3, -5) === 15, "positive product");
    shouldBeTrue(1 / mul(-1, 0) === -Infinity, "negative zero product");
}
