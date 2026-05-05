// Public API for goroutines
'use strict';

const { go, goyield, goid, threadid } = require('internal/goroutine');

module.exports = {
  go,
  goyield,
  goid,
  threadid,
};

