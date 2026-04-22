const {go} = require('goroutine');
const N = 1000;


// SharedArrayBuffer + Atomics — единственный безопасный способ
// разделять счётчик между горутин-тредами и main thread.
const sab = new SharedArrayBuffer(4);
const counter = new Int32Array(sab);

console.log('Lock free: ', Atomics.isLockFree(counter.BYTES_PER_ELEMENT));

const start = performance.now();

class User {
    name = 'default';
    age = 'default';
    sex = 'default';

    constructor(data) {
        Object.assign(this, data);
    }
}

function worker() {
    // const obj = {}
    const arr = [];
    // const instance = new User();
    let sum = 0;
    for (let j = 0; j < 1_000_000; j++) {
        sum += j * 10;
        const len = arr.push(sum);
        // obj['prop'] = sum
        // obj.curr = arr[len - 1];
    }
    Atomics.add(counter, 0, 1);
}

for (let i = 0; i < N; i++) {
    go(worker);
}

function waitAll() {
    const done = Atomics.load(counter, 0);
    if (done < N) {
        console.log('Curr', done);
        setTimeout(waitAll, 500);
    } else {
        const took = performance.now() - start;
        console.log(`All ${done} goroutines done OK in ${took.toFixed(0)} ms`);
    }
}

setTimeout(waitAll, 500);
