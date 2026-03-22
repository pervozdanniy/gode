const { go, yield: goyield, goid } = require('goroutine');

console.log('=== Phase 2: Multi-threaded Test ===\n');

let results = [];

for (let i = 1; i <= 5; i++) {
  go((n) => {
    results.push(`G${goid()} start`);
    goyield();
    results.push(`G${goid()} done`);
  }, i);
}

setTimeout(() => {
  console.log(results.join('\n'));
  console.log(`\nTotal: ${results.length} events`);
  console.log(results.length === 10 ? '\n✅ ALL GOROUTINES COMPLETED' : '\n❌ MISSING EVENTS');
  process.exit(0);
}, 500);

