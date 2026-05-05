// Minimal test - just check goroutine infrastructure
const {go, goid} = require('goroutine');

console.log('Memory Goroutine Test');
console.log('======================\n');

console.log('Main thread goid:', goid());

console.log('\nCreating goroutines...');

function getObj() {
    return {opa: 'jopa'}
}

function getFunc() {
    return (obj, id) => ({...obj, id, res: 3});
}

const N = 10000;
for (let i = 0; i < N; i++) {
    go(() => {
        // while (true) {}

        const id = goid();
        // const tid = threadid();
        for (let j = 0; j < 1000000; j++) {
            const opa = getObj();
            const fn = getFunc();
            const topa = fn(opa, id);
        }

    });
}

console.log('Created', N, 'goroutines');
console.log('\nWaiting for execution...');

Promise.resolve().then(() => {
    console.log('All goroutines should have completed by now');
});

// Wait
setTimeout(() => {
    console.log('\nDone');
}, 100);
