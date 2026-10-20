// Small character classes must compare the whole character, not just its low
// byte: U+012C has the low byte of ",", U+0161 that of "a". Covers classes of
// one to nine members, negated and case-insensitive classes, and 8-bit and
// 16-bit subject strings, each run enough times to reach the RegExp JIT.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

const classes = [",", ",;", ",;:", ",;:.", "aeiou", "aeiouy", ",;:.!?-", ",;:.!?-'", ",;:.!?-'\""];

function membersOf(cls) { return [...cls]; }

function lowByteTwins(cls) {
    // Characters above Latin-1 that share a member's low byte.
    return membersOf(cls).map(c => String.fromCharCode(0x100 | c.charCodeAt(0)));
}

for (let iteration = 0; iteration < 200; ++iteration) {
    for (const cls of classes) {
        const escaped = cls.replace(/[\\\]\-^]/g, "\\$&");
        const positive = new RegExp("[" + escaped + "]", "g");
        const negative = new RegExp("[^" + escaped + "]", "g");
        const insensitive = new RegExp("[" + escaped + "]", "gi");

        const members = membersOf(cls);
        const twins = lowByteTwins(cls);

        // 16-bit subject: twins interleaved with members.
        const wide = twins.join("") + members.join("") + twins.join("");
        shouldBe(wide.match(positive).join(""), members.join(""), "positive /" + positive.source + "/ on 16-bit string");
        shouldBe(wide.match(negative).join(""), twins.join("") + twins.join(""), "negative /" + negative.source + "/ on 16-bit string");
        shouldBe(wide.match(insensitive).length, members.length, "case-insensitive /" + insensitive.source + "/ on 16-bit string");

        for (const twin of twins) {
            positive.lastIndex = 0;
            shouldBe(positive.test(twin), false, "/" + positive.source + "/ must not match U+" + twin.charCodeAt(0).toString(16));
        }

        // 8-bit subject: members surrounded by non-members.
        const narrow = "#" + members.join("#") + "#";
        shouldBe(narrow.match(positive).join(""), members.join(""), "positive /" + positive.source + "/ on 8-bit string");
        shouldBe(narrow.match(negative).join(""), "#".repeat(members.length + 1), "negative /" + negative.source + "/ on 8-bit string");
    }
}
