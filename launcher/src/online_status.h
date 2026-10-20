#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace launcher {

// Probes the FM2 relay (tools/fm2-relay) in the background. The probe is a
// relay JOIN answered by a CHALLENGE, which creates no session on the server.
class OnlineStatus {
 public:
  enum class State { kChecking, kAvailable, kUnavailable };

  ~OnlineStatus();
  void Start(const std::string& server);  // host:port; restarts any probe
  State state() const { return state_.load(); }

 private:
  std::atomic<State> state_{State::kChecking};
  std::thread probe_;
};

}  // namespace launcher
