// Included inside lan.cpp's private namespace, before the XNet hooks.
//
// Relay mode (--fm2_relay=host:port) tunnels every game datagram through an
// FM2 relay server that acts as a virtual System Link segment: each client is
// given a 10.68.x.y address and broadcasts reach every client in the same room,
// so the native System Link browser lists every session on the server. The
// protocol is documented in tools/fm2-relay/README.md.
constexpr uint8_t kRelayMagic = 0xF2;
enum RelayType : uint8_t {
  kRelayJoin = 0x01,
  kRelayChallenge = 0x02,
  kRelayJoin2 = 0x03,
  kRelayWelcome = 0x04,
  kRelayBind = 0x05,
  kRelayPing = 0x06,
  kRelayError = 0x08,
  kRelayDataOut = 0x10,
  kRelayDataIn = 0x11,
  kRelayDataOutRedundant = 0x12,
  kRelayDataInRedundant = 0x13,
};
constexpr int kRelayHeader = 8; // magic, type, IPv4, port
constexpr int kRelayJoinSize = 64;
constexpr size_t kRelayMaxRoom = 32;
constexpr int kRelayDatagram = 2048;
// Loss recovery. Players' uplinks drop a few percent of UDP datagrams and
// FM2's DirectPlay stalls ~200 ms on every lost unreliable frame. When the
// server advertises it, each unicast datagram carries a per-destination
// sequence number plus copies of the previous two; receivers deliver anything
// new, oldest first, and drop duplicates.
constexpr uint8_t kRelayCapsRedundant = 0x01;
constexpr size_t kRelayCopies = 2;
constexpr size_t kRelayRedundantBudget = 1200; // stay below the path MTU

struct RelayDatagram {
  Ip from{};
  uint16_t port = 0;
  std::vector<uint8_t> payload;
};
using RelayStreamKey = std::tuple<uint32_t, uint32_t, uint16_t>; // handle, ip, port
struct RelayOutbound {
  uint16_t seq = 0;
  std::deque<std::vector<uint8_t>> history; // newest first
};
struct RelayInbound {
  bool started = false;
  uint16_t last = 0;
};

struct RelaySocket {
  SOCKET native = INVALID_SOCKET;
  uint16_t port = 0;
  bool connected = false;
  Ip peer{};
  uint16_t peer_port = 0;
};
struct RelayState {
  bool active = false;
  sockaddr_in server{};
  Ip address{};
  std::array<uint8_t, 8> token{};
  SOCKET control = INVALID_SOCKET;
  std::mutex mutex;
  std::map<uint32_t, RelaySocket> sockets;
  uint16_t next_port = 49152;
  std::thread keepalive;
  std::condition_variable wake;
  bool stopping = false;
  bool redundant = false; // server supports loss-recovery framing
  std::map<RelayStreamKey, RelayOutbound> outbound;
  std::map<RelayStreamKey, RelayInbound> inbound;
  std::map<uint32_t, std::deque<RelayDatagram>> pending; // recovered, undelivered
  std::atomic<uint64_t> recovered{0};
};
RelayState relay;

bool RelayActive() { return relay.active; }
bool InRelaySubnet(uint32_t ip) { return (ip >> 16) == 0x0A44; } // 10.68/16

void RelayPutHeader(uint8_t *header, uint8_t type, const Ip &ip,
                    uint16_t port) {
  header[0] = kRelayMagic;
  header[1] = type;
  std::memcpy(header + 2, ip.data(), 4);
  header[6] = uint8_t(port >> 8);
  header[7] = uint8_t(port);
}
bool RelayFromServer(const sockaddr_in &source) {
  return source.sin_addr.s_addr == relay.server.sin_addr.s_addr &&
         source.sin_port == relay.server.sin_port;
}
bool RelayUnicast(const Ip &to) {
  const uint32_t ip = IpValue(to);
  return ip != 0xffffffffu && (ip & 0xffffu) != 0xffffu;
}
// Builds the server-bound packet for one game datagram.
std::vector<uint8_t> RelayEncode(uint32_t handle, const Ip &to, uint16_t port,
                                 const uint8_t *data, size_t size) {
  std::vector<uint8_t> packet(kRelayHeader);
  if (!relay.redundant || !RelayUnicast(to)) {
    RelayPutHeader(packet.data(), kRelayDataOut, to, port);
    packet.insert(packet.end(), data, data + size);
    return packet;
  }
  RelayPutHeader(packet.data(), kRelayDataOutRedundant, to, port);
  std::lock_guard lock(relay.mutex);
  auto &stream = relay.outbound[{handle, IpValue(to), port}];
  const uint16_t seq = ++stream.seq;
  packet.push_back(uint8_t(seq >> 8));
  packet.push_back(uint8_t(seq));
  const size_t count = packet.size();
  packet.push_back(0);
  auto add = [&](const uint8_t *bytes, size_t n) {
    packet.push_back(uint8_t(n >> 8));
    packet.push_back(uint8_t(n));
    packet.insert(packet.end(), bytes, bytes + n);
    ++packet[count];
  };
  add(data, size);
  for (const auto &old : stream.history) {
    if (packet.size() + 2 + old.size() > kRelayRedundantBudget)
      break;
    add(old.data(), old.size());
  }
  stream.history.emplace_front(data, data + size);
  if (stream.history.size() > kRelayCopies)
    stream.history.pop_back();
  return packet;
}
// Datagrams to deliver from one server packet, oldest first. Empty for
// anything that is not new game traffic from the server.
std::vector<RelayDatagram> RelayDecode(uint32_t handle, const uint8_t *data,
                                       int size, const sockaddr_in &source) {
  std::vector<RelayDatagram> out;
  if (size < kRelayHeader || data[0] != kRelayMagic || !RelayFromServer(source))
    return out;
  Ip from{};
  std::memcpy(from.data(), data + 2, 4);
  const uint16_t port = uint16_t(data[6]) << 8 | data[7];
  if (data[1] == kRelayDataIn) {
    out.push_back({from, port, {data + kRelayHeader, data + size}});
    return out;
  }
  if (data[1] != kRelayDataInRedundant || size < kRelayHeader + 3)
    return out;
  const uint16_t seq = uint16_t(data[8]) << 8 | data[9];
  std::vector<std::pair<const uint8_t *, size_t>> entries; // newest first
  size_t offset = kRelayHeader + 3;
  for (unsigned i = 0; i < data[10] && offset + 2 <= size_t(size); ++i) {
    const size_t n = size_t(data[offset]) << 8 | data[offset + 1];
    offset += 2;
    if (offset + n > size_t(size))
      break;
    entries.emplace_back(data + offset, n);
    offset += n;
  }
  std::lock_guard lock(relay.mutex);
  auto &stream = relay.inbound[{handle, IpValue(from), port}];
  for (size_t i = entries.size(); i-- > 0;) {
    const uint16_t entry_seq = uint16_t(seq - i);
    const int16_t ahead = int16_t(uint16_t(entry_seq - stream.last));
    // Duplicates and stale copies are skipped; a far jump backwards means the
    // sender restarted its stream.
    if (stream.started && ahead <= 0 && ahead > -1024)
      continue;
    if (stream.started && i > 0)
      ++relay.recovered;
    stream.started = true;
    stream.last = entry_seq;
    out.push_back({from, port,
                   {entries[i].first, entries[i].first + entries[i].second}});
  }
  return out;
}
void RelayQueue(uint32_t handle, std::vector<RelayDatagram> &datagrams,
                size_t first) {
  if (first >= datagrams.size())
    return;
  std::lock_guard lock(relay.mutex);
  auto &queue = relay.pending[handle];
  for (size_t i = first; i < datagrams.size(); ++i)
    queue.push_back(std::move(datagrams[i]));
}
bool RelayTakePending(uint32_t handle, RelayDatagram &out) {
  std::lock_guard lock(relay.mutex);
  auto found = relay.pending.find(handle);
  if (found == relay.pending.end() || found->second.empty())
    return false;
  out = std::move(found->second.front());
  found->second.pop_front();
  return true;
}
void RelaySendControl(SOCKET socket, const uint8_t *data, int size) {
  sendto(socket, reinterpret_cast<const char *>(data), size, 0,
         reinterpret_cast<const sockaddr *>(&relay.server),
         sizeof(relay.server));
}
void RelaySendBind(SOCKET socket, uint16_t port) {
  uint8_t packet[12] = {kRelayMagic, kRelayBind};
  std::memcpy(packet + 2, relay.token.data(), 8);
  packet[10] = uint8_t(port >> 8);
  packet[11] = uint8_t(port);
  RelaySendControl(socket, packet, sizeof(packet));
}

bool RelayResolve(const std::string &target) {
  const auto colon = target.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == target.size())
    return false;
  const auto host = target.substr(0, colon), service = target.substr(colon + 1);
  addrinfo hints{}, *found = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &found) != 0 ||
      !found)
    return false;
  std::memcpy(&relay.server, found->ai_addr, sizeof(relay.server));
  freeaddrinfo(found);
  return true;
}
bool RelayValidRoom(const std::string &room) {
  return !room.empty() && room.size() <= kRelayMaxRoom &&
         std::all_of(room.begin(), room.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
         });
}
void RelayKeepalive() {
  std::unique_lock lock(relay.mutex);
  while (!relay.stopping) {
    relay.wake.wait_for(lock, std::chrono::seconds(10));
    if (relay.stopping)
      break;
    uint8_t ping[10] = {kRelayMagic, kRelayPing};
    std::memcpy(ping + 2, relay.token.data(), 8);
    RelaySendControl(relay.control, ping, sizeof(ping));
    // Refreshes the server binding and the player's NAT mapping per socket.
    for (auto &[handle, socket] : relay.sockets)
      RelaySendBind(socket.native, socket.port);
    if (const auto recovered = relay.recovered.exchange(0))
      REXLOG_INFO("FM2 relay recovered {} lost datagrams in the last 10 s",
                  recovered);
    uint8_t reply[256];
    sockaddr_in source{};
    int length = sizeof(source);
    int n;
    while ((n = recvfrom(relay.control, reinterpret_cast<char *>(reply),
                         sizeof(reply), 0,
                         reinterpret_cast<sockaddr *>(&source), &length)) >
           0) {
      if (n >= 10 && reply[0] == kRelayMagic && reply[1] == kRelayError &&
          RelayFromServer(source))
        REXLOG_ERROR("FM2 relay lost this session (server restarted?); "
                     "restart the game to play online again");
      length = sizeof(source);
    }
  }
}
// Joins a room and obtains this client's virtual address. Runs at startup,
// before guest code; on failure the caller falls back to offline play.
bool RelayStart(const std::string &target, const std::string &room) {
  if (!RelayValidRoom(room)) {
    REXLOG_ERROR("FM2 relay: room must be 1-32 characters of A-Z a-z 0-9 - _");
    return false;
  }
  WSADATA startup{};
  if (WSAStartup(MAKEWORD(2, 2), &startup) != 0)
    return false;
  if (!RelayResolve(target)) {
    REXLOG_ERROR("FM2 relay: cannot resolve '{}' (expected host:port)",
                 target);
    return false;
  }
  relay.control = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (relay.control == INVALID_SOCKET)
    return false;
  DWORD timeout = 500;
  setsockopt(relay.control, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout), sizeof(timeout));
  std::array<uint8_t, 8> nonce{};
  BCryptGenRandom(nullptr, nonce.data(), ULONG(nonce.size()),
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  std::array<uint8_t, 16> cookie{};
  bool have_cookie = false;
  for (int attempt = 0; attempt < 6; ++attempt) {
    // JOIN and JOIN2 are padded so replies are never larger than requests.
    uint8_t request[kRelayJoinSize]{};
    request[0] = kRelayMagic;
    request[1] = have_cookie ? kRelayJoin2 : kRelayJoin;
    std::memcpy(request + 2, nonce.data(), 8);
    size_t offset = 10;
    if (have_cookie) {
      std::memcpy(request + offset, cookie.data(), cookie.size());
      offset += cookie.size();
    }
    request[offset++] = uint8_t(room.size());
    std::memcpy(request + offset, room.data(), room.size());
    RelaySendControl(relay.control, request, sizeof(request));
    uint8_t reply[256];
    sockaddr_in source{};
    int length = sizeof(source);
    const int n =
        recvfrom(relay.control, reinterpret_cast<char *>(reply), sizeof(reply),
                 0, reinterpret_cast<sockaddr *>(&source), &length);
    if (n < 10 || reply[0] != kRelayMagic || !RelayFromServer(source) ||
        std::memcmp(reply + 2, nonce.data(), 8) != 0)
      continue;
    if (reply[1] == kRelayChallenge && n >= 26) {
      std::memcpy(cookie.data(), reply + 10, cookie.size());
      have_cookie = true;
      --attempt; // A challenge is progress, not a lost attempt.
    } else if (reply[1] == kRelayWelcome && n >= 22) {
      std::memcpy(relay.token.data(), reply + 10, 8);
      std::memcpy(relay.address.data(), reply + 18, 4);
      if (!InRelaySubnet(IpValue(relay.address)))
        return false;
      relay.redundant = n >= 23 && (reply[22] & kRelayCapsRedundant);
      u_long nonblocking = 1;
      ioctlsocket(relay.control, FIONBIO, &nonblocking);
      relay.active = true;
      relay.keepalive = std::thread(RelayKeepalive);
      REXLOG_WARN("FM2 relay: joined room '{}' on {} as {}{}", room, target,
                  IpText(relay.address),
                  relay.redundant ? " (loss recovery on)" : "");
      return true;
    } else if (reply[1] == kRelayError && n > 11) {
      REXLOG_ERROR("FM2 relay rejected the join: {}",
                   std::string(reinterpret_cast<char *>(reply) + 11,
                               size_t(n - 11)));
      return false;
    }
  }
  REXLOG_ERROR("FM2 relay: no answer from {}", target);
  return false;
}
void RelayStop() {
  {
    std::lock_guard lock(relay.mutex);
    relay.stopping = true;
    relay.wake.notify_all();
  }
  if (relay.keepalive.joinable())
    relay.keepalive.join();
  if (relay.control != INVALID_SOCKET)
    closesocket(relay.control);
  relay.control = INVALID_SOCKET;
}

// Binds the native socket to an ephemeral host port and registers the guest's
// virtual port with the server. Returns a WSA error code.
uint32_t RelayBindSocket(uint32_t handle, SOCKET native, uint16_t port) {
  sockaddr_in any{};
  any.sin_family = AF_INET;
  if (bind(native, reinterpret_cast<sockaddr *>(&any), sizeof(any)) ==
      SOCKET_ERROR)
    return uint32_t(WSAGetLastError());
  std::lock_guard lock(relay.mutex);
  if (!port)
    port = relay.next_port++;
  relay.sockets[handle] = {native, port};
  RelaySendBind(native, port);
  RelaySendBind(native, port); // Cheap insurance against a lost first bind.
  return 0;
}
std::optional<RelaySocket> RelayLookup(uint32_t handle) {
  std::lock_guard lock(relay.mutex);
  auto found = relay.sockets.find(handle);
  if (found == relay.sockets.end())
    return {};
  return found->second;
}
void RelayConnect(uint32_t handle, const Ip &peer, uint16_t port) {
  std::lock_guard lock(relay.mutex);
  auto &socket = relay.sockets[handle];
  socket.connected = true;
  socket.peer = peer;
  socket.peer_port = port;
}
void RelayForget(uint32_t handle) {
  std::lock_guard lock(relay.mutex);
  relay.sockets.erase(handle);
  relay.pending.erase(handle);
  std::erase_if(relay.outbound, [handle](const auto &e) {
    return std::get<0>(e.first) == handle;
  });
  std::erase_if(relay.inbound, [handle](const auto &e) {
    return std::get<0>(e.first) == handle;
  });
}
// Synchronous tunnelled send; returns payload bytes or SOCKET_ERROR.
int RelaySendTo(uint32_t handle, SOCKET native, const void *data, int size,
                const Ip &to, uint16_t port) {
  if (size < 0 || size > kRelayDatagram - kRelayHeader) {
    WSASetLastError(WSAEMSGSIZE);
    return SOCKET_ERROR;
  }
  const auto packet = RelayEncode(
      handle, to, port, static_cast<const uint8_t *>(data), size_t(size));
  const int sent =
      sendto(native, reinterpret_cast<const char *>(packet.data()),
             int(packet.size()), 0,
             reinterpret_cast<const sockaddr *>(&relay.server),
             sizeof(relay.server));
  return sent == SOCKET_ERROR ? SOCKET_ERROR : size;
}
// Synchronous tunnelled receive; skips non-game datagrams and serves datagrams
// recovered from earlier packets first.
int RelayReceiveFrom(uint32_t handle, SOCKET native, void *data, int size,
                     int flags, Ip &from, uint16_t &port) {
  RelayDatagram datagram;
  if (!RelayTakePending(handle, datagram)) {
    uint8_t packet[kRelayDatagram];
    for (;;) {
      sockaddr_in source{};
      int length = sizeof(source);
      const int n = recvfrom(native, reinterpret_cast<char *>(packet),
                             sizeof(packet), flags,
                             reinterpret_cast<sockaddr *>(&source), &length);
      if (n == SOCKET_ERROR)
        return SOCKET_ERROR;
      auto datagrams = RelayDecode(handle, packet, n, source);
      if (datagrams.empty())
        continue;
      datagram = std::move(datagrams.front());
      RelayQueue(handle, datagrams, 1);
      break;
    }
  }
  from = datagram.from;
  port = datagram.port;
  const int payload = std::min(int(datagram.payload.size()), size);
  std::memcpy(data, datagram.payload.data(), size_t(payload));
  return payload;
}
