const {Worker, isMainThread, workerData} = require('worker_threads');
const N = 100;
const PROCS = +process.env.GOMAXPROCS ?? 4


const start = performance.now();

class User {
    name = 'default';
    age = 'default';
    sex = 'default';

    constructor(data) {
        Object.assign(this, data);
    }
}

function waitAll(counter) {
    const done = Atomics.load(counter, 0);
    if (done < N) {
        console.log('Curr', done);
        setTimeout(waitAll, 100, counter);
    } else {
        const took = performance.now() - start;
        console.log(`All ${done} goroutines done OK in ${took.toFixed(0)} ms`);
    }
}

if (isMainThread) {
    const sab = new SharedArrayBuffer(4);
    const counter = new Int32Array(sab);
    for (let i = 0; i < PROCS; i++) {
        new Worker(__filename, {workerData: sab});
    }


    setTimeout(waitAll, 500, counter);
} else {
    const counter = new Int32Array(workerData);
    for (let i = 0; i < N / PROCS; i++) {
        // const obj = {}
        // const arr = [];
        // const instance = new User();
        let sum = 0;
        for (let j = 0; j < 1_000_000; j++) {
            sum += j * 10;
            // const len = arr.push(sum);
            // obj['prop'] = sum
            // obj.curr = arr[len - 1];
        }
        Atomics.add(counter, 0, 1);
    }

}


