'use strict';

const assert = require('assert');
const { go, goid } = require('goroutine');

// Test: goid() inside a goroutine returns a positive ID.
let innerGoid = -1;

go(() => {
  innerGoid = goid();
});

setTimeout(() => {
  assert.ok(innerGoid > 0, `goid() inside goroutine should be > 0, got ${innerGoid}`);
  console.log('✅ test-goroutine-goid passed');
}, 500);

