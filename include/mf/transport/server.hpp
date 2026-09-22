#pragma once

// Bounded TCP server: one accept thread, a fixed worker pool and a bounded
// connection queue. Admission is refused (and counted) once the queue is full,
// so an unresponsive peer cannot make the process grow without limit.
//
// Shutdown ordering is deliberate: mark stopping, close the listener to release
// the accept call, shut down every live connection so no worker is blocked in a
// read, then join outside every lock.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"
#include "mf/core/time.hpp"
#include "mf/transport/frame.hpp"
#include "mf/transport/socket.hpp"

namespace mf {

struct ServerStats {
  std::uint64_t accepted{0};
  std::uint64_t rejected{0};
  std::uint64_t handled{0};
  std::uint64_t failures{0};
  std::uint64_t frames_in{0};
  std::uint64_t frames_out{0};
  std::uint64_t bytes_in{0};
  std::uint64_t bytes_out{0};
};

class TcpServer {
 public:
  struct Options {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{0};
    std::uint32_t workers{4};
    std::uint32_t max_queued{limits::kMaxQueuedRequests};
    Nanos io_timeout{limits::kDefaultRpcDeadlineNanos * 4};
    std::size_t max_payload{limits::kMaxFrameBytes};
  };

  /// Returns the response frame for a request. Returning a failed status makes
  /// the server answer with an Error frame; the handler must never be called
  /// while the server holds its own lock.
  using Handler = std::function<Status(const Frame& request, Frame& response)>;

  TcpServer(Options options, Handler handler);
  ~TcpServer();
  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  [[nodiscard]] Status start();
  [[nodiscard]] Status stop();
  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  /// Resolved listening port (useful when the configured port was 0).
  [[nodiscard]] std::uint16_t port() const noexcept { return port_.load(); }
  [[nodiscard]] ServerStats stats() const;

 private:
  void accept_loop();
  void worker_loop();
  void serve(Socket connection);

  Options options_;
  Handler handler_;
  Socket listener_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Socket> queue_;
  std::set<Socket*> live_;
  std::vector<std::thread> workers_;
  std::thread acceptor_;
  bool stopping_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint16_t> port_{0};
  std::atomic<std::uint64_t> accepted_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint64_t> handled_{0};
  std::atomic<std::uint64_t> failures_{0};
  std::atomic<std::uint64_t> frames_in_{0};
  std::atomic<std::uint64_t> frames_out_{0};
  std::atomic<std::uint64_t> bytes_in_{0};
  std::atomic<std::uint64_t> bytes_out_{0};
};

}  // namespace mf
