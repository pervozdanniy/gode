// Test with GOMAXPROCS=1 (single M-thread)
process.env.NODE_GOMAXPROCS = '1';

const { go } = require('goroutine');

console.log('Single Thread Goroutine Test\n');

go(() => {
  console.log('✅ GOROUTINE 1 WORKS!');
});

go(() => {
  console.log('✅ GOROUTINE 2 WORKS!');
});

go(() => {
  console.log('✅ GOROUTINE 3 WORKS!');
});

setTimeout(() => {
  console.log('\nDone');
  process.exit(0);
}, 2000);

