const {go, goid} = require('goroutine');
const N = 100;
const sab = new SharedArrayBuffer(4);
const counter = new Int32Array(sab);
console.log('Testing GC safepoint with main thread allocation...');
const start = performance.now();
function worker() {
    let sum = 0;
    for (let j = 0; j < 1_000_000; j++) {
        sum += j * 10;
    }
    Atomics.add(counter, 0, 1);
}
for (let i = 0; i < N; i++) {
    go(worker);
}
// Active allocation on main thread — triggers GC which requires M-threads to park.
function waitAll() {
    // Main thread allocates heavily → triggers GC → needs safepoint from M-threads.
    for (let i = 0; i < 100_000; i++) {
        const a = {i, nested: {x: i, arr: [i, i+1, i+2]}};
    }
    const done = Atomics.load(counter, 0);
    if (done < N) {
        console.log('Curr', done);
        setTimeout(waitAll, 50);
    } else {
        const took = performance.now() - start;
        console.log(`All ${done} goroutines done OK in ${took.toFixed(0)} ms`);
    }
}
setTimeout(waitAll, 100);
