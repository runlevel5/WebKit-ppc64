// Int52 addition with a constant small enough to be an instruction immediate must
// detect overflow in both directions, and must not report overflow when a negative
// value becomes non-negative (the existing *-constant-overflow-recovery tests only
// use constants too large for an immediate).

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + ", expected " + expected);
}

function addOne(x) { return fiatInt52(x) + 1; }
function addSmall(x) { return fiatInt52(x) + 1000; }
function subOne(x) { return fiatInt52(x) - 1; }
function subSmall(x) { return fiatInt52(x) - 1000; }
noInline(addOne);
noInline(addSmall);
noInline(subOne);
noInline(subSmall);

const INT52_MAX = 2 ** 51 - 1;
const INT52_MIN = -(2 ** 51);

for (let i = 0; i < testLoopCount; ++i) {
    const x = 2 ** 40 + i;
    shouldBe(addOne(x), x + 1);
    shouldBe(addSmall(x), x + 1000);
    shouldBe(subOne(-x), -x - 1);
    shouldBe(subSmall(-x), -x - 1000);
    // A negative operand whose sum is non-negative is not an overflow.
    shouldBe(addOne(-1), 0);
    shouldBe(subOne(0), -1);
}

// Overflow past the Int52 range in both directions.
shouldBe(addOne(INT52_MAX), INT52_MAX + 1);
shouldBe(addSmall(INT52_MAX - 500), INT52_MAX + 500);
shouldBe(subOne(INT52_MIN), INT52_MIN - 1);
shouldBe(subSmall(INT52_MIN + 500), INT52_MIN - 500);
shouldBe(addSmall(-999), 1);
shouldBe(subSmall(999), -1);
