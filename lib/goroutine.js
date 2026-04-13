// Public API for goroutines
'use strict';

const { go, yield: goyield, goid, threadid } = require('internal/goroutine');

module.exports = {
  go,
  yield: goyield,
  goid,
  threadid,
};

