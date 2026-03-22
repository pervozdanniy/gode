// Test goroutines
const { go, yield: goyield, goid } = require('goroutine');

console.log('Testing Node.js Goroutines');
console.log('===========================\n');

console.log('Test 1: Get current goroutine ID');
console.log('  Current goid:', goid());

console.log('\nTest 2: Creating a simple goroutine');
const id1 = go(() => {
  console.log('  [Goroutine] Hello from goroutine!');
  console.log('  [Goroutine] My ID:', goid());
});
console.log('  Created goroutine with ID:', id1);

console.log('\nTest 3: Goroutine with arguments');
const id2 = go((name, num) => {
  console.log(`  [Goroutine] Hello ${name}, number: ${num}`);
}, 'World', 42);
console.log('  Created goroutine with ID:', id2);

console.log('\nTest 4: Multiple goroutines');
for (let i = 0; i < 5; i++) {
  const id = go((num) => {
    console.log(`  [Goroutine ${num}] Running...`);
  }, i);
  console.log(`  Created goroutine ${i} with ID: ${id}`);
}

console.log('\n===========================');
console.log('✅ Tests completed!');
console.log('\nNote: Goroutines are scheduled in M threads.');
console.log('Context switching not yet implemented - functions execute directly.');
console.log('Check for [Runtime] and [M*] logs above.');

// Give time for goroutines to execute
setTimeout(() => {
  process.exit(0);
}, 1000);

