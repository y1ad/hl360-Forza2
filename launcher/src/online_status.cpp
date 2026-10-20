#include "online_status.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>

namespace launcher {

namespace {

constexpr uint8_t kMagic = 0xF2, kJoin = 0x01;
constexpr char kRoom[] = "public";

bool Probe(const std::string& server) {
  const auto colon = server.rfind(':');
  if (colon == std::string::npos) return false;
  const std::string host = server.substr(0, colon), port = server.substr(colon + 1);
  addrinfo hints{}, *found = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found) return false;
  sockaddr_in target{};
  std::memcpy(&target, found->ai_addr, sizeof(target));
  freeaddrinfo(found);

  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return false;
  DWORD timeout = 700;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  std::array<uint8_t, 8> nonce{};
  std::random_device random;
  for (auto& b : nonce) b = uint8_t(random());
  // Same layout as the game's JOIN: magic, type, nonce, room; padded to 64.
  std::array<uint8_t, 64> join{};
  join[0] = kMagic;
  join[1] = kJoin;
  std::memcpy(join.data() + 2, nonce.data(), nonce.size());
  join[10] = uint8_t(sizeof(kRoom) - 1);
  std::memcpy(join.data() + 11, kRoom, sizeof(kRoom) - 1);
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
    sendto(s, reinterpret_cast<const char*>(join.data()), int(join.size()), 0,
           reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    uint8_t reply[128];
    const int n = recv(s, reinterpret_cast<char*>(reply), sizeof(reply), 0);
    ok = n >= 10 && reply[0] == kMagic && std::memcmp(reply + 2, nonce.data(), nonce.size()) == 0;
  }
  closesocket(s);
  return ok;
}

}  // namespace

OnlineStatus::~OnlineStatus() {
  if (probe_.joinable()) probe_.join();
}

void OnlineStatus::Start(const std::string& server) {
  if (probe_.joinable()) probe_.join();
  state_ = State::kChecking;
  probe_ = std::thread([this, server] {
    WSADATA wsa{};
    const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    state_ = started && Probe(server) ? State::kAvailable : State::kUnavailable;
    if (started) WSACleanup();
  });
}

}  // namespace launcher
