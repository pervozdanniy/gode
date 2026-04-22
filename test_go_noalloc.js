const {go} = require('goroutine');
const N = 100_000;
const sab = new SharedArrayBuffer(4);
const counter = new Int32Array(sab);
const start = performance.now();

function worker() {
    // let sum = 0;
    // for (let j = 0; j < 1_000_000; j++) {
    //     sum += j * 10;
    // }
    Atomics.add(counter, 0, 1);
}

for (let i = 0; i < N; i++) {
    go(worker);
}

function waitAll() {
    const done = Atomics.load(counter, 0);
    if (done < N) {
        console.log('Curr', done);
        setTimeout(waitAll, 50);
    } else {
        const took = performance.now() - start;
        console.log(`All ${done} goroutines done OK in ${took.toFixed(0)} ms`);
    }
}

setTimeout(waitAll, 5000);

