#include "env-inl.h"
#include "node.h"
#include "node_binding.h"
#include "node_external_reference.h"
#include "util-inl.h"
#include "v8.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

#include "goroutine/runtime.h"
#include "goroutine/scheduler.h"
#include "goroutine/g.h"
#include "goroutine/context.h"

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

    const char* env_gomaxprocs = getenv("NODE_GOMAXPROCS");
    if (!env_gomaxprocs) env_gomaxprocs = getenv("GOMAXPROCS");
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

// goprint(...args) — goroutine-safe print.
// Converts all arguments to strings (like console.log), enqueues the message,
// and signals the main thread to flush. Safe to call from any goroutine thread.
// The actual write() happens on the main thread — no libuv races.
void Goprint(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  std::string output;
  for (int i = 0; i < args.Length(); i++) {
    if (i > 0) output += ' ';
    v8::String::Utf8Value str(isolate, args[i]);
    if (*str) output += *str;
  }
  output += '\n';

  goroutine::Runtime::GetInstance()->EnqueuePrint(std::move(output));
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context,
                void* priv) {
  SetMethod(context, target, "go", Go);
  SetMethod(context, target, "yield", Yield);
  SetMethod(context, target, "goid", Goid);
  SetMethod(context, target, "threadid", Threadid);
  SetMethod(context, target, "goprint", Goprint);
}

void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(Go);
  registry->Register(Yield);
  registry->Register(Goid);
  registry->Register(Threadid);
  registry->Register(Goprint);
}

}  // namespace goroutine_wrap
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(goroutine,
                                    node::goroutine_wrap::Initialize)
NODE_BINDING_EXTERNAL_REFERENCE(
    goroutine, node::goroutine_wrap::RegisterExternalReferences)
