'use strict';

const assert = require('assert');
const { go } = require('goroutine');

// Test: goroutines can allocate on the heap (strings, arrays, objects).
let result;

go(() => {
  const s = 'hello' + ' ' + 'world';
  const arr = [1, 2, 3];
  const obj = { a: 1, b: s };
  result = { s, arr, obj };
});

setTimeout(() => {
  assert.strictEqual(result.s, 'hello world');
  assert.deepStrictEqual(result.arr, [1, 2, 3]);
  assert.strictEqual(result.obj.a, 1);
  console.log('✅ test-goroutine-heap passed');
}, 500);

