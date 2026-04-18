const { go } = require('goroutine');
const N = 5000;

// SharedArrayBuffer + Atomics — единственный безопасный способ
// разделять счётчик между горутин-тредами и main thread.
const sab = new SharedArrayBuffer(4);
const counter = new Int32Array(sab);

console.log(Atomics.isLockFree(counter.BYTES_PER_ELEMENT), 'Counter is lock-free');

const start = performance.now();
const fn = () => {
  const obj = { opa: 'jopa' }
  const arr = new Array(40000);
  for (let j = 0; j < 100000; j++) {}
  Atomics.add(counter, 0, 1);
};

for (let i = 0; i < N; i++) {
  go(fn);
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
setTimeout(waitAll, 50);
