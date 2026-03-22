// Public API for goroutines
'use strict';

const { go, yield: goyield, goid } = require('internal/goroutine');

module.exports = {
  go,
  yield: goyield,
  goid,
};

