'use strict';

const assert = require('assert');
const { go, goid } = require('goroutine');

// Test: go() creates a goroutine and returns a numeric ID.
const id = go(() => {});
assert.strictEqual(typeof id, 'number');
assert.ok(id > 0, `expected positive goid, got ${id}`);

// Test: goid() on main thread returns 0 (no active goroutine).
assert.strictEqual(goid(), 0);

// Test: go() with arguments passes them correctly.
let received;
go((a, b) => {
  received = a + b;
}, 3, 7);

// Test: multiple goroutines get unique IDs.
const ids = new Set();
for (let i = 0; i < 10; i++) {
  ids.add(go(() => {}));
}
assert.strictEqual(ids.size, 10, 'goroutine IDs must be unique');

// Allow goroutines to execute, then verify.
setTimeout(() => {
  assert.strictEqual(received, 10);
  console.log('✅ test-goroutine-basic passed');
}, 500);

