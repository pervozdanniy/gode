// Minimal goroutine test - no console.log inside goroutines
const { go, goid } = require('goroutine');

console.log('=== Minimal Goroutine Test ===');
console.log('Main goid:', goid());

// Test 1: Goroutine with empty function (no I/O)
console.log('\nTest 1: Empty goroutine');
const id1 = go(() => {
  // Do nothing - just test creation and execution
  let x = 1 + 2;
});
console.log('Created goroutine:', id1);

// Wait and exit
setTimeout(() => {
  console.log('\n=== Done ===');
  process.exit(0);
}, 2000);

