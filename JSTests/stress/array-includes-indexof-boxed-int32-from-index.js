// The DFG passes the start index to the Array.prototype.includes / indexOf C++
// operations as an int32_t. When fromIndex arrives as a boxed int32 (here, a
// property load), the register can still hold the number tag in its upper half;
// ABIs where the caller widens 32-bit arguments (PPC64 ELFv2, RISC-V) need it
// extended before the call, or the operation indexes with the tagged value.
const o = {}, p = {};
const array = [1, 2, p, {}, o, 5];
const zero = { k: 0 }, one = { k: 1 };

function includes(a, v, h) { return a.includes(v, h.k); }
function indexOf(a, v, h) { return a.indexOf(v, h.k); }
noInline(includes);
noInline(indexOf);

for (let i = 0; i < testLoopCount; ++i) {
    const h = (i & 1) ? zero : one;
    if (!includes(array, o, h))
        throw new Error("includes: bad result at " + i);
    if (indexOf(array, p, h) !== 2)
        throw new Error("indexOf: bad result at " + i);
}
