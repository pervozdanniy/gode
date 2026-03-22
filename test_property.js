// Test: Direct property access from M-thread
const { go } = require('goroutine');

global.testCounter = 0;

console.log('Test: Direct property access\n');
console.log('Initial testCounter:', global.testCounter);

go(() => {
  // Try to access global property directly
  global.testCounter = 42;
});

setTimeout(() => {
  console.log('Final testCounter:', global.testCounter);
  console.log(global.testCounter === 42 ? '✅ SUCCESS' : '❌ FAILED');
  process.exit(0);
}, 2000);

