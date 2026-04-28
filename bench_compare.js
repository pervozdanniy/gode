// Benchmark: allocation-heavy vs compute-only
const {go} = require('goroutine');

const sab = new SharedArrayBuffer(4);
const counter = new Int32Array(sab);

function workAlloc() {
    const arr = [];
    let sum = 0;
    for (let j = 0; j < 1_000_000; j++) {
        sum += j * 10;
        arr.push(sum);
    }
    Atomics.add(counter, 0, 1);
}

function workCompute() {
    let sum = 0;
    for (let j = 0; j < 10_000_000; j++) {
        sum += j;
    }
    Atomics.add(counter, 0, 1);
    return sum;
}

const mode = process.argv[2] || 'goroutine-alloc';
const N = 100;

if (mode === 'inline-alloc') {
    const start = performance.now();
    for (let i = 0; i < N; i++) workAlloc();
    console.log(`inline-alloc: ${(performance.now() - start).toFixed(0)} ms`);
} else if (mode === 'inline-compute') {
    const start = performance.now();
    for (let i = 0; i < N; i++) workCompute();
    console.log(`inline-compute: ${(performance.now() - start).toFixed(0)} ms`);
} else {
    const work = mode === 'goroutine-compute' ? workCompute : workAlloc;
    const start = performance.now();
    for (let i = 0; i < N; i++) go(work);
    function waitAll() {
        if (Atomics.load(counter, 0) < N) {
            setTimeout(waitAll, 50);
        } else {
            console.log(`${mode}: ${(performance.now() - start).toFixed(0)} ms`);
        }
    }
    setTimeout(waitAll, 100);
}

