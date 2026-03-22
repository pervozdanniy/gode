// Minimal test - just check goroutine infrastructure
const { go, goid } = require('goroutine');

console.log('Minimal Goroutine Test');
console.log('======================\n');

console.log('Main thread goid:', goid());

console.log('\nCreating 1 goroutine...');
const id = go(() => {
  console.log('SUCCESS: Goroutine executed!');
  console.log('  My goid:', goid());
});

console.log('Created goroutine with ID:', id);
console.log('\nWaiting for execution...');

// Wait
setTimeout(() => {
  console.log('\nDone');
  process.exit(0);
}, 2000);

