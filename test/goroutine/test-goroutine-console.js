'use strict';

const assert = require('assert');
const { go, goid } = require('goroutine');

// Test: goroutine can call console.log (involves many V8 internals).
let logged = false;
const origLog = console.log;
console.log = (...args) => {
  if (args[0] === 'GOROUTINE_OUTPUT') logged = true;
  origLog.apply(console, args);
};

go(() => {
  console.log('GOROUTINE_OUTPUT');
});

setTimeout(() => {
  console.log = origLog;
  assert.ok(logged, 'goroutine should have called console.log');
  console.log('✅ test-goroutine-console passed');
}, 500);

