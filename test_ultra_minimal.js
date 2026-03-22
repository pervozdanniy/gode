// Ultra minimal test
const { go } = require('goroutine');

console.log('Ultra Minimal Test\n');

go(() => {
  console.log('✅ GOROUTINE WORKS!');
});

setTimeout(() => process.exit(0), 1000);

