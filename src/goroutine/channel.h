#ifndef SRC_GOROUTINE_CHANNEL_H_
#define SRC_GOROUTINE_CHANNEL_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <deque>
#include <vector>
#include "v8.h"
#include "g.h"
#include "node_mutex.h"

namespace node {
namespace goroutine {

// Channel for communication between goroutines
class Channel {
 public:
  explicit Channel(v8::Isolate* isolate, uint32_t buffer_size = 0);
  ~Channel();

  // Send value (blocks if buffer full)
  void Send(v8::Local<v8::Value> value);

  // Receive value (blocks if buffer empty)
  v8::Local<v8::Value> Recv(v8::Isolate* isolate);

  // Try send (non-blocking, returns false if full)
  bool TrySend(v8::Local<v8::Value> value);

  // Try receive (non-blocking, returns empty Maybe if empty)
  v8::MaybeLocal<v8::Value> TryRecv(v8::Isolate* isolate);

  // Close channel
  void Close();
  bool IsClosed() const { return closed_; }

  // Buffer info
  uint32_t buffer_size() const { return buffer_size_; }
  uint32_t length() const;

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

 private:
  v8::Isolate* isolate_;
  uint32_t buffer_size_;  // 0 for unbuffered
  bool closed_ = false;

  // Buffered values
  Mutex mutex_;
  std::deque<v8::Global<v8::Value>> buffer_;

  // Waiting goroutines
  std::vector<G*> send_waiters_;  // Blocked on send
  std::vector<G*> recv_waiters_;  // Blocked on receive
};

}  // namespace goroutine
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
#endif  // SRC_GOROUTINE_CHANNEL_H_

