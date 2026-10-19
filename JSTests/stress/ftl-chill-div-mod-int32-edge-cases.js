// (a / b) | 0 and (a % b) | 0 on int32 inputs compile to ArithDiv / ArithMod in
// Arith::Unchecked mode on targets whose DFG fixup speculates Int32 division,
// which FTL lowers to B3's chill Div / chill Mod. Those must give x/0 == 0,
// INT_MIN/-1 == INT_MIN, x%0 == 0 and INT_MIN%-1 == 0 without relying on what the
// hardware divide instruction happens to return for those inputs, since some ISAs
// leave the result undefined. Targets that take the double path instead must of
// course produce the same answers.

function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(label + ": expected " + expected + ", got " + actual);
}

function chillDiv(a, b) {
    return (a / b) | 0;
}
noInline(chillDiv);

function chillMod(a, b) {
    return (a % b) | 0;
}
noInline(chillMod);

const INT_MIN = -2147483648;
const INT_MAX = 2147483647;

const cases = [
    [7, 2],
    [-7, 2],
    [7, -2],
    [INT_MAX, 3],
    [INT_MIN, 3],
    [INT_MIN, 1],
    [INT_MIN, -1],
    [INT_MIN, 0],
    [INT_MAX, -1],
    [INT_MAX, 0],
    [0, 0],
    [0, -1],
    [-1, 0],
    [42, 0],
    [-42, -1],
    [INT_MIN, INT_MIN],
    [INT_MIN, INT_MAX],
];

// Compute the expectations once, before anything is hot, so they come from the
// interpreter rather than from the code under test.
const expected = cases.map(([a, b]) => [(a / b) | 0, (a % b) | 0]);

for (let i = 0; i < testLoopCount; ++i) {
    for (let j = 0; j < cases.length; ++j) {
        const [a, b] = cases[j];
        shouldBe(chillDiv(a, b), expected[j][0], "(" + a + " / " + b + ") | 0");
        shouldBe(chillMod(a, b), expected[j][1], "(" + a + " % " + b + ") | 0");
    }
}

shouldBe(chillDiv(INT_MIN, -1), INT_MIN, "INT_MIN / -1");
shouldBe(chillDiv(5, 0), 0, "5 / 0");
shouldBe(chillMod(INT_MIN, -1), 0, "INT_MIN % -1");
shouldBe(chillMod(5, 0), 0, "5 % 0");
