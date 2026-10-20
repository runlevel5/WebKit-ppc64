//@ runDefault("--sampleCCode=1")
// With C frames sampled, the profiler must still produce stack traces while
// most of the time is spent in C++ (JSON.parse, sorting), and walking the C
// frames must not crash.
if (platformSupportsSamplingProfiler()) {
    const json = JSON.stringify(Array.from({ length: 5000 }, (_, i) => ({ id: i, name: "item" + i, tags: ["a", "b"] })));
    function work() {
        let n = 0;
        for (let i = 0; i < 40; ++i) {
            n += JSON.parse(json).length;
            n += Array.from({ length: 5000 }, (_, x) => String((x * 7919) % 10007)).sort().length;
        }
        return n;
    }

    startSamplingProfiler();
    let traces = 0;
    for (let round = 0; round < 5 && traces < 50; ++round) {
        work();
        traces += samplingProfilerStackTraces().traces.length;
    }
    if (traces < 50)
        throw new Error("expected at least 50 stack traces with --sampleCCode, got " + traces);
}
