// Test console.log inside goroutine
const { go } = require('goroutine');

console.log('=== Console.log in Goroutine Test ===');

const id = go(() => {
  // This calls Node.js internals which may have uncompiled inner functions
  console.log('[Goroutine] Hello from goroutine!');
});
console.log('Created goroutine:', id);

setTimeout(() => {
  console.log('\n=== Done ===');
  process.exit(0);
}, 2000);

