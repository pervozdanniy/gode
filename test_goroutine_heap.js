// Test goroutine with heap allocation (string concatenation)
const { go, goid } = require('goroutine');

console.log('=== Goroutine Heap Allocation Test ===');

// Test 1: Pure computation (no heap alloc) - should work
console.log('\nTest 1: Pure Smi computation');
const id1 = go(() => {
  let x = 1 + 2;
  let y = x * 10;
});
console.log('Created goroutine:', id1);

// Give time for goroutine to execute
setTimeout(() => {
  // Test 2: String creation (heap alloc) - might crash
  console.log('\nTest 2: String creation in goroutine');
  const id2 = go(() => {
    let s = "hello" + " world";
  });
  console.log('Created goroutine:', id2);

  setTimeout(() => {
    console.log('\n=== Done ===');
    process.exit(0);
  }, 2000);
}, 2000);

