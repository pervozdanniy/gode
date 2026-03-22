// Test goroutines basic functionality
const { go, yield: goyield, goid } = require('./lib/goroutine');

console.log('Testing Node.js Goroutines');
console.log('===========================\n');

// Test 1: Simple goroutine
console.log('Test 1: Creating a simple goroutine');
go(() => {
  console.log('  [Goroutine 1] Hello from goroutine!');
  console.log('  [Goroutine 1] My ID:', goid());
});

// Test 2: Goroutine with arguments
console.log('\nTest 2: Goroutine with arguments');
go((name, num) => {
  console.log(`  [Goroutine 2] Hello ${name}, number: ${num}`);
}, 'World', 42);

// Test 3: Multiple goroutines
console.log('\nTest 3: Multiple goroutines');
for (let i = 0; i < 5; i++) {
  go((id) => {
    console.log(`  [Goroutine ${id + 3}] Running with ID: ${id}`);
  }, i);
}

// Give time for goroutines to execute
setTimeout(() => {
  console.log('\n===========================');
  console.log('Test completed');
  process.exit(0);
}, 1000);

