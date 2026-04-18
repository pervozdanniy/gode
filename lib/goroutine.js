// Public API for goroutines
'use strict';

const { go, yield: goyield, goid, threadid, goprint } = require('internal/goroutine');

module.exports = {
  go,
  goyield,
  goid,
  threadid,
  // Goroutine-safe print — use instead of console.log inside goroutines.
  // Enqueues output to be written by the main thread (no libuv races).
  goprint,
};

