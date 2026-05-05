#include "env-inl.h"
#include "node.h"
#include "node_binding.h"
#include "node_external_reference.h"
#include "util-inl.h"
#include "v8.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

#include "goroutine/runtime.h"
#include "goroutine/scheduler.h"
#include "goroutine/g.h"
#include "goroutine/context.h"

// Goroutine TLS flags — defined in deps/v8/src/execution/goroutine-thread.cc
// v8_goroutine_thread : true while an M-thread is running JS
// v8_goroutine_real_isolate : the real Isolate* (M-threads have a per-M
//   IsolateData as their r13 root; this TLS holds the actual Isolate pointer)
extern thread_local __attribute__((tls_model("initial-exec")))
    bool v8_goroutine_thread;
extern thread_local __attribute__((tls_model("initial-exec")))
    void* v8_goroutine_real_isolate;

#define GWRAP_TRACE(fmt, ...) do {} while(0)

namespace node {
namespace goroutine_wrap {

using v8::Array;
using v8::Context;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Value;

// go(func, ...args) — create and schedule a new goroutine.
void Go(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope handle_scope(isolate);

  if (args.Length() < 1 || !args[0]->IsFunction()) {
    isolate->ThrowException(
        v8::Exception::TypeError(
            String::NewFromUtf8Literal(isolate,
                "First argument must be a function")));
    return;
  }

  // Lazy-init runtime on first go() call.
  goroutine::Runtime* runtime = goroutine::Runtime::GetInstance();
  if (!runtime->IsInitialized()) {
    uint32_t gomaxprocs = 1;

    const char* env_gomaxprocs = getenv("GOMAXPROCS");
    if (env_gomaxprocs) {
      int parsed = atoi(env_gomaxprocs);
      if (parsed > 0 && parsed <= 256) {
        gomaxprocs = static_cast<uint32_t>(parsed);
      }
    }

    Environment* env = Environment::GetCurrent(args);
    runtime->Init(gomaxprocs, env->event_loop(), isolate);

    // Register cleanup so workers call isolate->Exit() before teardown.
    env->AtExit([](void*) {
      goroutine::Runtime::GetInstance()->Shutdown();
    }, nullptr);
  }

  Local<v8::Function> func = args[0].As<v8::Function>();

  // Collect extra arguments into an array.
  Local<Array> func_args = Array::New(isolate, args.Length() - 1);
  for (int i = 1; i < args.Length(); i++) {
    func_args->Set(isolate->GetCurrentContext(), i - 1, args[i]).Check();
  }

  // Create goroutine and schedule it.
  goroutine::G* g = new goroutine::G(isolate, func, func_args);
  GWRAP_TRACE("go(): created G%llu, scheduling...", (unsigned long long)g->goid());
  goroutine::Scheduler::GetInstance()->Schedule(g);
  // Wake worker M-threads so they can pick up the goroutine.
  goroutine::Runtime::GetInstance()->NotifyGoroutineAvailable();

  args.GetReturnValue().Set(
      Number::New(isolate, static_cast<double>(g->goid())));
}

// yield() — explicit cooperative yield.
void Yield(const FunctionCallbackInfo<Value>& args) {
  goroutine::YieldG();
}

// goid() — get current goroutine ID.
void Goid(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  goroutine::G* g = goroutine::CurrentG();
  double id = g ? static_cast<double>(g->goid()) : 0;
  args.GetReturnValue().Set(Number::New(isolate, id));
}

// threadid() — get OS thread ID (Linux gettid) of the calling M thread.
void Threadid(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
  args.GetReturnValue().Set(Number::New(isolate, static_cast<double>(tid)));
}

// writeFdSync(fd, stringOrBuffer) — goroutine-safe direct fd write.
//
// Accepts either a JS String or an ArrayBufferView (Buffer/Uint8Array).
//
// String path:
//   Uses String::Utf8Value(real_isolate, str) under a static mutex.
//   The real Isolate pointer comes from v8_goroutine_real_isolate TLS — needed
//   because on M-threads args.GetIsolate() returns a per-M IsolateData (fake),
//   and String::Flatten(fake_isolate) tries to call fake_isolate->factory()
//   at the wrong struct offset → SIGSEGV / UNREACHABLE.
//
// ArrayBufferView path:
//   Reads raw bytes from the backing store and calls write(2) directly.
//   No V8 string API needed.
void WriteFdSync(const FunctionCallbackInfo<Value>& args) {
  if (args.Length() < 2) return;

  // Fix up Isolate for M-threads: args.GetIsolate() returns per-M IsolateData;
  // use the real Isolate* stored in v8_goroutine_real_isolate instead.
  Isolate* isolate = args.GetIsolate();
  if (v8_goroutine_thread && v8_goroutine_real_isolate) {
    isolate = reinterpret_cast<Isolate*>(v8_goroutine_real_isolate);
  }

  int fd = args[0]->Int32Value(isolate->GetCurrentContext()).FromMaybe(1);

  if (args[1]->IsString()) {
    // String path — mutex serialises String::Flatten + write(2) across
    // multiple M-threads (also prevents interleaved output on the fd).
    static std::mutex write_mutex;
    std::lock_guard<std::mutex> lock(write_mutex);

    String::Utf8Value utf8(isolate, args[1]);
    if (*utf8 == nullptr || utf8.length() == 0) return;

    const char* buf = *utf8;
    ssize_t remaining = static_cast<ssize_t>(utf8.length());
    while (remaining > 0) {
      ssize_t n = ::write(fd, buf, static_cast<size_t>(remaining));
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      buf += n;
      remaining -= n;
    }
    return;
  }

  // ArrayBufferView path (Buffer / Uint8Array / etc.)
  if (!args[1]->IsArrayBufferView()) return;
  Local<v8::ArrayBufferView> view = args[1].As<v8::ArrayBufferView>();
  size_t byte_length = view->ByteLength();
  if (byte_length == 0) return;

  std::shared_ptr<v8::BackingStore> backing = view->Buffer()->GetBackingStore();
  const char* buf =
      static_cast<const char*>(backing->Data()) + view->ByteOffset();
  ssize_t remaining = static_cast<ssize_t>(byte_length);

  // write(2) is thread-safe; retry on EINTR.
  while (remaining > 0) {
    ssize_t n = ::write(fd, buf, static_cast<size_t>(remaining));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    buf += n;
    remaining -= n;
  }
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  SetMethod(context, target, "go", Go);
  SetMethod(context, target, "yield", Yield);
  SetMethod(context, target, "goid", Goid);
  SetMethod(context, target, "threadid", Threadid);
  SetMethod(context, target, "writeFdSync", WriteFdSync);
}

void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(Go);
  registry->Register(Yield);
  registry->Register(Goid);
  registry->Register(Threadid);
  registry->Register(WriteFdSync);
}

}  // namespace goroutine_wrap
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(goroutine,
                                    node::goroutine_wrap::Initialize)
NODE_BINDING_EXTERNAL_REFERENCE(
    goroutine, node::goroutine_wrap::RegisterExternalReferences)
