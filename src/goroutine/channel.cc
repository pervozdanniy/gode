#include "channel.h"
#include "scheduler.h"
#include <cstdio>

namespace node {
namespace goroutine {

Channel::Channel(v8::Isolate* isolate, uint32_t buffer_size)
    : isolate_(isolate), buffer_size_(buffer_size) {
}

Channel::~Channel() {
  Close();
}

void Channel::Send(v8::Local<v8::Value> value) {
  Mutex::ScopedLock lock(mutex_);

  if (closed_) {
    // Panic: send on closed channel
    fprintf(stderr, "panic: send on closed channel\n");
    std::abort();
  }

  // TODO: Implement blocking send with Park/Ready
  // For now, just buffer or panic if full

  if (buffer_.size() < buffer_size_ || buffer_size_ == 0) {
    buffer_.emplace_back(isolate_, value);

    // Wake up a receiver if any
    if (!recv_waiters_.empty()) {
      G* g = recv_waiters_.back();
      recv_waiters_.pop_back();
      Scheduler::GetInstance()->Ready(g);
    }
  } else {
    fprintf(stderr, "TODO: Block sender (buffer full)\n");
  }
}

v8::Local<v8::Value> Channel::Recv(v8::Isolate* isolate) {
  Mutex::ScopedLock lock(mutex_);

  // TODO: Implement blocking receive with Park/Ready
  // For now, just return from buffer or undefined

  if (!buffer_.empty()) {
    v8::Global<v8::Value> global_val = std::move(buffer_.front());
    buffer_.pop_front();

    v8::Local<v8::Value> val = global_val.Get(isolate);
    global_val.Reset();

    // Wake up a sender if any
    if (!send_waiters_.empty()) {
      G* g = send_waiters_.back();
      send_waiters_.pop_back();
      Scheduler::GetInstance()->Ready(g);
    }

    return val;
  }

  if (closed_) {
    return v8::Undefined(isolate);
  }

  fprintf(stderr, "TODO: Block receiver (buffer empty)\n");
  return v8::Undefined(isolate);
}

bool Channel::TrySend(v8::Local<v8::Value> value) {
  Mutex::ScopedLock lock(mutex_);

  if (closed_ || buffer_.size() >= buffer_size_) {
    return false;
  }

  buffer_.emplace_back(isolate_, value);
  return true;
}

v8::MaybeLocal<v8::Value> Channel::TryRecv(v8::Isolate* isolate) {
  Mutex::ScopedLock lock(mutex_);

  if (buffer_.empty()) {
    return v8::MaybeLocal<v8::Value>();
  }

  v8::Global<v8::Value> global_val = std::move(buffer_.front());
  buffer_.pop_front();

  v8::Local<v8::Value> val = global_val.Get(isolate);
  global_val.Reset();

  return val;
}

void Channel::Close() {
  Mutex::ScopedLock lock(mutex_);

  if (closed_) return;
  closed_ = true;

  // Wake up all waiting goroutines
  Scheduler* sched = Scheduler::GetInstance();

  for (G* g : send_waiters_) {
    sched->Ready(g);
  }
  send_waiters_.clear();

  for (G* g : recv_waiters_) {
    sched->Ready(g);
  }
  recv_waiters_.clear();
}

uint32_t Channel::length() const {
  Mutex::ScopedLock lock(const_cast<Mutex&>(mutex_));
  return buffer_.size();
}

}  // namespace goroutine
}  // namespace node

