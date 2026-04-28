// Single iteration benchmark - goroutine vs inline
const {go, goid} = require('goroutine');

const sab = new SharedArrayBuffer(4);
const counter = new Int32Array(sab);

function work() {
    const arr = [];
    let sum = 0;
    for (let j = 0; j < 1_000_000; j++) {
        sum += j * 10;
        arr.push(sum);
    }
    Atomics.add(counter, 0, 1);
}

const mode = process.argv[2] || 'goroutine';

if (mode === 'inline') {
    // Run directly on main thread - same as worker thread does
    const start = performance.now();
    for (let i = 0; i < 100; i++) {
        work();
    }
    console.log(`Inline: ${(performance.now() - start).toFixed(0)} ms`);
} else {
    // Run as goroutines
    const start = performance.now();
    for (let i = 0; i < 100; i++) {
        go(work);
    }
    function waitAll() {
        if (Atomics.load(counter, 0) < 100) {
            setTimeout(waitAll, 50);
        } else {
            console.log(`Goroutine: ${(performance.now() - start).toFixed(0)} ms`);
        }
    }
    setTimeout(waitAll, 100);
}

