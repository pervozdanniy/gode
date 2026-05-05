const {go, goid} = require('goroutine');
const N = 100;


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
    console.log('Working...', goid());
    const map = new Map();
    function inner(key, value) {
        map.set(`prop_${key}`, value);
    }

    const obj = {}

    const arr = [];
    // const arr = new Array(1_000_000);
    // const instance = new User();
    let sum = 0;
    for (let j = 0; j < 1_000_000; j++) {

        sum += j * 10;
        // arr[j] = sum;
        const len = arr.push(sum);
        // obj[`prop_${j}`] = arr[len - 1]
        // inner(j, sum);
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
        setTimeout(waitAll, 100);
    } else {
        const took = performance.now() - start;
        console.log(`All ${done} goroutines done OK in ${took.toFixed(0)} ms`);
    }
}

setTimeout(waitAll, 500);
