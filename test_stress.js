const { go, yield: goyield, goid } = require('goroutine');

const GOROUTINES = 1000;
const results = [];
let completed = 0;

console.log(`=== Stress Test: ${GOROUTINES} goroutines, GOMAXPROCS=${process.env.NODE_GOMAXPROCS || 1} ===\n`);

const start = Date.now();

for (let i = 0; i < GOROUTINES; i++) {
  go(() => {
    const id = goid();
    results.push(`G${id} start`);
    goyield();
    results.push(`G${id} done`);
    completed++;
  });
}

setTimeout(() => {
  const elapsed = Date.now() - start;
  console.log(`Completed: ${completed}/${GOROUTINES}`);
  console.log(`Events: ${results.length}/${GOROUTINES * 2}`);
  console.log(`Time: ${elapsed}ms`);

  if (completed === GOROUTINES && results.length === GOROUTINES * 2) {
    console.log('\n✅ ALL PASSED');
  } else {
    console.log('\n❌ FAILED');
  }

  process.exit(0);
}, 2000);

