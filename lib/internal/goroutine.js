'use strict';

const {
  go: _go,
  yield: _yield,
  goid: _goid,
} = internalBinding('goroutine');

// Main go() function - creates and schedules a goroutine
function go(fn, ...args) {
  if (typeof fn !== 'function') {
    throw new TypeError('First argument must be a function');
  }
  
  return _go(fn, ...args);
}

// Explicit yield - cooperative scheduling
function goyield() {
  return _yield();
}

// Get current goroutine ID
function goid() {
  return _goid();
}

module.exports = {
  go,
  yield: goyield,
  goid,
};
