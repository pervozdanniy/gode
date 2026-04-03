'use strict';

const assert = require('assert');
const { go } = require('goroutine');

// Test: many goroutines all complete without crash.
const N = 100;
let counter = 0;

for (let i = 0; i < N; i++) {
  go(() => {
    counter++;
  });
}

setTimeout(() => {
  assert.strictEqual(counter, N, `expected ${N} goroutines to run, got ${counter}`);
  console.log(`✅ test-goroutine-many passed (${N} goroutines)`);
}, 2000);

