const { go, goyield, goid } = require('goroutine');

console.log('=== Context Switch Test ===\n');

go(() => {
  console.log(`[G${goid()}] start`);
  goyield();
  console.log(`[G${goid()}] resumed after yield`);
});

go(() => {
  console.log(`[G${goid()}] start`);
  goyield();
  console.log(`[G${goid()}] resumed after yield`);
});

go(() => {
  console.log(`[G${goid()}] runs to completion`);
});

setTimeout(() => {
  console.log('\n=== Done ===');
  process.exit(0);
}, 500);

