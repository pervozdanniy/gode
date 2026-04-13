'use strict';

const {
  go: _go,
  yield: _yield,
  goid: _goid,
  threadid: _threadid,
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

// Get OS thread ID of the M thread executing this goroutine
function threadid() {
  return _threadid();
}

module.exports = {
  go,
  yield: goyield,
  goid,
  threadid,
};
