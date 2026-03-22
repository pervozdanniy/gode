// Test without console.log
const { go } = require('goroutine');

let executed = false;

console.log('Test: Goroutine without console.log\n');

go(() => {
  executed = true;  // Just set a flag
});

setTimeout(() => {
  console.log('executed =', executed);
  process.exit(0);
}, 2000);

