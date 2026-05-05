'use strict';

const {
  go: _go,
  yield: _yield,
  goid: _goid,
  threadid: _threadid,
  writeFdSync: _writeFdSync,
} = internalBinding('goroutine');

// Goroutine-safe console write: joins args with spaces, no regex, no colors.
// Used by console.log/error patches below.
function _goroutineConsoleWrite(fd, args) {
  let str = '';
  for (let i = 0; i < args.length; i++) {
    if (i > 0) str += ' ';
    const a = args[i];
    str += (typeof a === 'string') ? a : String(a);
  }
  _writeFdSync(fd, str + '\n');
}

// Patch console.log / console.error (and aliases) for goroutine M-threads.
//
// Root cause of crash: Console.prototype.log → kGetInspectOptions →
// getColorDepth() → /^screen|^xterm|.../.test(env.TERM)
// → IrregexpInterpreter::RawMatch(fake_isolate) → SIGSEGV.
//
// Fix: on M-threads skip the Console machinery entirely and write directly
// via writeFdSync (write(2) syscall, no libuv, no regex).
{
  const _origLog   = console.log;
  const _origError = console.error;
  const _origWarn  = console.warn;
  const _origInfo  = console.info;
  const _origDebug = console.debug;

  function goroutineLog(...args) {
    if (_goid() > 0) { _goroutineConsoleWrite(process.stdout.fd, args); return; }
    return _origLog.apply(this, args);
  }
  function goroutineError(...args) {
    if (_goid() > 0) { _goroutineConsoleWrite(process.stderr.fd, args); return; }
    return _origError.apply(this, args);
  }

  console.log   = goroutineLog;
  console.info  = goroutineLog;
  console.debug = goroutineLog;
  console.warn  = goroutineError;
  console.error = goroutineError;
}

// Also patch process.stdout / process.stderr .write for any code that
// calls them directly (not via console.*) from a goroutine M-thread.
// This avoids Buffer.from() + shared pool race on the write path.
{
  const _stdoutWrite = process.stdout.write.bind(process.stdout);
  const _stderrWrite = process.stderr.write.bind(process.stderr);

  function goroutineSafeWrite(fd, originalWrite, chunk, encoding, callback) {
    if (_goid() > 0) {
      if (typeof encoding === 'function') {
        callback = encoding;
      }
      _writeFdSync(fd, chunk);
      if (typeof callback === 'function') callback(null);
      return true;
    }
    return originalWrite(chunk, encoding, callback);
  }

  process.stdout.write = function goroutineStdoutWrite(chunk, encoding, callback) {
    return goroutineSafeWrite(process.stdout.fd, _stdoutWrite, chunk, encoding, callback);
  };

  process.stderr.write = function goroutineStderrWrite(chunk, encoding, callback) {
    return goroutineSafeWrite(process.stderr.fd, _stderrWrite, chunk, encoding, callback);
  };
}

// Main go() function - creates and schedules a goroutine.
// Wraps the user function in a try-catch so unhandled exceptions don't crash
// the process. Caught exceptions are printed to stderr (which now uses
// goroutine-safe writeSync) and the goroutine exits gracefully.
function go(fn, ...args) {
  if (typeof fn !== 'function') {
    throw new TypeError('First argument must be a function');
  }
  return _go(() => {
    try {
      fn(...args);
    } catch (e) {
      const id = _goid();
      let msg = 'unknown error';
      try {
        // DO NOT access e.stack here — the .stack accessor triggers
        // ErrorStackGetter → FormatStackTrace → JS execution (CallSite
        // constructors, Error.prepareStackTrace).  That JS execution on
        // M-threads hits DisallowJavascriptExecutionScope / UNREACHABLE.
        // Instead read only the simple data properties (name, message).
        if (e != null && typeof e === 'object') {
          const name = (typeof e.name === 'string') ? e.name : 'Error';
          const message = (typeof e.message === 'string') ? e.message : '';
          msg = message ? (name + ': ' + message) : name;
        } else {
          msg = String(e);
        }
      } catch {}
      _goroutineConsoleWrite(process.stderr.fd, [`goroutine ${id} panic:`, msg]);
    }
  });
}

function goyield() { return _yield(); }
function goid()    { return _goid();  }
function threadid(){ return _threadid(); }

module.exports = { go, goyield, goid, threadid };

