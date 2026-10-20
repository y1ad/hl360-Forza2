#if defined(_WIN32)
#include "lan.h"
#include "lan_address.h"
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <algorithm>
#include <atomic>
#include <bcrypt.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <windows.h>

REXCVAR_DEFINE_BOOL(
    fm2_lan_selftest, false, "FM2/LAN",
    "Exchange a test handshake between two game processes, then exit");
REXCVAR_DEFINE_BOOL(fm2_lan, false, "FM2/LAN",
                    "Experimental direct LAN transport; not Xbox Live");
REXCVAR_DEFINE_STRING(fm2_lan_ip, "", "FM2/LAN",
                      "Local IPv4 address to bind and advertise");
REXCVAR_DEFINE_STRING(
    fm2_lan_peer, "", "FM2/LAN",
    "Optional peer IPv4; directs discovery broadcasts to this peer");
REXCVAR_DEFINE_STRING(
    fm2_relay, "", "FM2/LAN",
    "FM2 relay server host:port; tunnels System Link through it (online play)");
REXCVAR_DEFINE_INT32(fm2_lan_port, 2302, "FM2/LAN",
                     "Advertised System Link port")
    .range(1024, 65535);

namespace forza2::lan {
namespace {
using namespace rex::ppc;
using rex::system::XSocket;
constexpr uint32_t kInvalid = 10022;
struct Peer {
  Address address{};
  KeyId key{};
};
std::mutex state_mutex;
std::map<KeyId, std::array<uint8_t, 16>> keys;
std::map<uint32_t, Peer> peers;
thread_local uint32_t guest_lr = 0; // Return address of the current hooked call.
#include "lan_relay.inc"

Ip LocalIp() {
  if (RelayActive())
    return relay.address;
  auto ip = ParseIp(REXCVAR_GET(fm2_lan_ip));
  if (!ip || !IsUsable(*ip))
    throw std::runtime_error("FM2 LAN requires --fm2_lan_ip=<local IPv4>");
  return *ip;
}
std::optional<Ip> PeerIp() {
  if (RelayActive())
    return {}; // The relay fans broadcasts out to the room.
  const auto text = REXCVAR_GET(fm2_lan_peer);
  if (text.empty())
    return {};
  auto ip = ParseIp(text);
  if (!ip || !IsUsable(*ip))
    throw std::runtime_error("Invalid --fm2_lan_peer IPv4");
  return ip;
}
Address LocalAddress() {
  return MakeAddress(LocalIp(), uint16_t(REXCVAR_GET(fm2_lan_port)));
}
void SocketError() {
  const auto error = uint32_t(WSAGetLastError());
  if (rex::system::XThread::GetCurrentThread())
    rex::system::XThread::SetLastError(error);
  else
    REXLOG_DEBUG("FM2 LAN host-side socket error {}", error);
}
SOCKET Native(uint32_t handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(handle);
  if (!socket) {
    WSASetLastError(WSAENOTSOCK);
    SocketError();
    return INVALID_SOCKET;
  }
  return SOCKET(socket->native_handle());
}
sockaddr_in NativeAddress(const rex::system::XSOCKADDR_IN &guest,
                          bool bind_address) {
  sockaddr_in a{};
  a.sin_family = uint16_t(guest.sin_family);
  a.sin_port = htons(uint16_t(guest.sin_port));
  Ip ip = IpBytes(uint32_t(guest.sin_addr));
  if (bind_address && IpValue(ip) == 0)
    ip = LocalIp();
  if (!bind_address)
    ip = Destination(ip, PeerIp());
  std::memcpy(&a.sin_addr.s_addr, ip.data(), ip.size());
  return a;
}
u32 Link(u32) { return 0x01 | 0x02 | 0x08; }
u32 TitleAddress(u32, ppc_ptr_t<Address> out) {
  if (!out)
    return 0x8000; // XNET_GET_XNADDR_TROUBLESHOOT
  *out = LocalAddress();
  return 0x04 | 0x08; // Ethernet + static address, no Xbox Live status.
}
u32 CreateKey(u32, mapped_void id, mapped_void key) {
  if (!id || !key)
    return kInvalid;
  uint8_t bytes[24];
  if (BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    return 8;
  bytes[0] &= 0x0f; // System Link key ID namespace.
  std::memcpy(id, bytes, 8);
  std::memcpy(key, bytes + 8, 16);
  return 0;
}
// The runtime fills XNetRandom with a constant, so both peers would derive the
// same nonces/IDs and the game can reject the other side as itself.
u32 Random(u32, mapped_void buffer, u32 length) {
  if (!buffer && uint32_t(length))
    return kInvalid;
  if (uint32_t(length) &&
      BCryptGenRandom(nullptr, static_cast<PUCHAR>(buffer.host_address()),
                      ULONG(uint32_t(length)),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    return 8;
  static std::atomic<unsigned> logged{0};
  if (logged.fetch_add(1) < 16)
    REXLOG_INFO("FM2 LAN XNetRandom {} bytes", uint32_t(length));
  return 0;
}
u32 RegisterKey(u32, mapped_void id, mapped_void key) {
  if (!id || !key)
    return kInvalid;
  KeyId session{};
  std::array<uint8_t, 16> exchange{};
  std::memcpy(session.data(), id, 8);
  std::memcpy(exchange.data(), key, 16);
  std::lock_guard lock(state_mutex);
  keys[session] = exchange;
  REXLOG_INFO("FM2 LAN registered session key ({} active)", keys.size());
  return 0;
}
u32 UnregisterKey(u32, mapped_void id) {
  if (!id)
    return kInvalid;
  KeyId session{};
  std::memcpy(session.data(), id, 8);
  std::lock_guard lock(state_mutex);
  keys.erase(session);
  for (auto it = peers.begin(); it != peers.end();) {
    if (it->second.key == session)
      it = peers.erase(it);
    else
      ++it;
  }
  return 0;
}
u32 ToIp(u32, ppc_ptr_t<Address> address, mapped_void id, mapped_void out) {
  if (!address || !id || !out || !IsUsable(address->local))
    return kInvalid;
  Peer p{};
  p.address = *address;
  std::memcpy(p.key.data(), id, 8);
  {
    std::lock_guard lock(state_mutex);
    if (!keys.contains(p.key))
      return kInvalid;
    peers[IpValue(p.address.local)] = p;
  }
  // in_addr is a four-byte network-order value, not a guest-endian integer.
  std::memcpy(out, p.address.local.data(), 4);
  REXLOG_INFO("FM2 LAN resolved peer {} lr={:08X}", IpText(p.address.local),
              guest_lr);
  return 0;
}
// Plain UDP does not create the console's secure-address mapping. In a
// configured direct pair, the sole active session identifies incoming traffic.
// Never guess a key for arbitrary sources or when multiple sessions are active.
auto FindPeer(uint32_t value) {
  auto found = peers.find(value);
  if (found == peers.end() && keys.size() == 1 && RelayActive() &&
      InRelaySubnet(value) && value != IpValue(relay.address)) {
    // Relay addresses are assigned by the server and every client derives its
    // XNADDR from its address with MakeAddress, so the mapping is exact.
    Peer peer{MakeAddress(IpBytes(value), uint16_t(REXCVAR_GET(fm2_lan_port))),
              keys.begin()->first};
    found = peers.emplace(value, peer).first;
    REXLOG_INFO("FM2 relay mapped room peer {} to the active session",
                IpText(IpBytes(value)));
  }
  if (found == peers.end() && keys.size() == 1) {
    auto configured = PeerIp();
    if (configured && IpValue(*configured) == value) {
      Peer peer{MakeAddress(*configured, uint16_t(REXCVAR_GET(fm2_lan_port))),
                keys.begin()->first};
      found = peers.emplace(value, peer).first;
      REXLOG_INFO(
          "FM2 LAN mapped configured incoming peer {} to the active session",
          IpText(*configured));
    }
  }
  return found;
}
// The key ID output is optional: FM2's DirectPlay layer passes null when it
// only needs the XNADDR to build the host URL, and treats failure as OOM.
u32 FromIp(u32, u32 ip, ppc_ptr_t<Address> out, mapped_void id) {
  if (!out)
    return kInvalid;
  const uint32_t value = ip;
  std::lock_guard lock(state_mutex);
  auto found = FindPeer(value);
  static std::atomic<unsigned> logged{0};
  if (logged.fetch_add(1) < 64)
    REXLOG_INFO("FM2 LAN InAddrToXnAddr {} -> {} lr={:08X}",
                IpText(IpBytes(value)),
                found != peers.end() ? "peer" : "unknown", guest_lr);
  if (found != peers.end()) {
    *out = found->second.address;
    if (id)
      std::memcpy(id, found->second.key.data(), 8);
    return 0;
  }
  if (value == IpValue(LocalIp()) && !keys.empty()) {
    *out = LocalAddress();
    if (id)
      std::memcpy(id, keys.begin()->first.data(), 8);
    return 0;
  }
  return kInvalid;
}
u32 Connect(u32, u32 ip) {
  REXLOG_INFO("FM2 LAN XNetConnect address {:08X} lr={:08X}", uint32_t(ip),
              guest_lr);
  if (uint32_t(ip) == 0xffffffffu)
    return 0; // Connectionless discovery.
  std::lock_guard lock(state_mutex);
  return FindPeer(uint32_t(ip)) != peers.end() ? 0 : kInvalid;
}
u32 ConnectStatus(u32, u32 ip) {
  if (uint32_t(ip) == 0xffffffffu)
    return 2; // Broadcast route is usable.
  static std::atomic<unsigned> logged_status{0};
  if (logged_status.fetch_add(1) < 12)
    REXLOG_INFO("FM2 LAN connection route queried {}",
                IpText(IpBytes(uint32_t(ip))));
  std::lock_guard lock(state_mutex);
  return FindPeer(uint32_t(ip)) != peers.end() ? 2 : 0;
}
u32 UnregisterIp(u32, u32 ip) {
  std::lock_guard lock(state_mutex);
  peers.erase(uint32_t(ip));
  return 0;
}
u32 AddressString(u32, u32 ip, mapped_string out, u32 size) {
  if (!out || !size)
    return kInvalid;
  const auto text = IpText(IpBytes(uint32_t(ip)));
  if (uint32_t(size) <= text.size())
    return kInvalid;
  std::memcpy(out, text.c_str(), text.size() + 1);
  return 0;
}
u32 Bind(u32, u32 handle, ppc_ptr_t<rex::system::XSOCKADDR_IN> address,
         u32 len) {
  if (!address || uint32_t(len) < 16 || address->sin_family != AF_INET) {
    WSASetLastError(WSAEINVAL);
    SocketError();
    return uint32_t(-1);
  }
  SOCKET socket = Native(handle);
  if (socket == INVALID_SOCKET)
    return uint32_t(-1);
  if (RelayActive()) {
    const auto error =
        RelayBindSocket(handle, socket, uint16_t(address->sin_port));
    if (error) {
      WSASetLastError(int(error));
      SocketError();
      return uint32_t(-1);
    }
    REXLOG_INFO("FM2 relay bound {}:{}", IpText(LocalIp()),
                RelayLookup(handle)->port);
    return 0;
  }
  auto a = NativeAddress(*address, true);
  int result = bind(socket, reinterpret_cast<sockaddr *>(&a), sizeof(a));
  if (result == SOCKET_ERROR)
    SocketError();
  else
    REXLOG_INFO("FM2 LAN bound {}:{}", IpText(LocalIp()),
                uint16_t(address->sin_port));
  return uint32_t(result);
}
u32 GetSockName(u32, u32 handle, ppc_ptr_t<rex::system::XSOCKADDR_IN> out,
                mapped_u32 length) {
  if (!out || !length || uint32_t(*length) < 16) {
    WSASetLastError(WSAEFAULT);
    SocketError();
    return uint32_t(-1);
  }
  SOCKET socket = Native(handle);
  if (socket == INVALID_SOCKET)
    return uint32_t(-1);
  if (RelayActive()) {
    // Report the virtual endpoint peers address us by, not the host socket.
    const auto bound = RelayLookup(handle);
    *out = {};
    out->sin_family = AF_INET;
    out->sin_port = bound ? bound->port : 0;
    out->sin_addr = bound ? IpValue(LocalIp()) : 0;
    *length = 16;
    return 0;
  }
  sockaddr_in address{};
  int n = sizeof(address);
  if (getsockname(socket, reinterpret_cast<sockaddr *>(&address), &n) ==
      SOCKET_ERROR) {
    SocketError();
    return uint32_t(-1);
  }
  *out = {};
  out->sin_family = address.sin_family;
  out->sin_port = ntohs(address.sin_port);
  out->sin_addr = ntohl(address.sin_addr.s_addr);
  *length = 16;
  return 0;
}
u32 GetSockOpt(u32, u32 handle, u32 level, u32 option, mapped_void out,
               mapped_u32 length) {
  if (!out || !length || !uint32_t(*length) || uint32_t(*length) > 4096) {
    WSASetLastError(WSAEFAULT);
    SocketError();
    return uint32_t(-1);
  }
  SOCKET socket = Native(handle);
  if (socket == INVALID_SOCKET)
    return uint32_t(-1);
  std::array<char, 4096> value{};
  int n = int(*length);
  if (getsockopt(socket, int(level), int(option), value.data(), &n) ==
      SOCKET_ERROR) {
    SocketError();
    return uint32_t(-1);
  }
  if (n == 4) {
    uint32_t integer;
    std::memcpy(&integer, value.data(), 4);
    *out.as<rex::be<uint32_t> *>() = integer;
  } else
    std::memcpy(out, value.data(), n);
  *length = uint32_t(n);
  return 0;
}
u32 SendTo(u32, u32 handle, mapped_void data, u32 size, u32 flags,
           ppc_ptr_t<rex::system::XSOCKADDR_IN> to, u32 len) {
  if (!data || !to || uint32_t(len) < 16 || to->sin_family != AF_INET) {
    WSASetLastError(WSAEINVAL);
    SocketError();
    return uint32_t(-1);
  }
  SOCKET socket = Native(handle);
  if (socket == INVALID_SOCKET)
    return uint32_t(-1);
  if (RelayActive()) {
    const int sent =
        RelaySendTo(handle, socket, data.host_address(), int(size),
                    IpBytes(uint32_t(to->sin_addr)), uint16_t(to->sin_port));
    if (sent == SOCKET_ERROR)
      SocketError();
    return uint32_t(sent);
  }
  auto a = NativeAddress(*to, false);
  int result = sendto(
      socket, reinterpret_cast<const char *>(data.host_address()), int(size),
      int(flags), reinterpret_cast<sockaddr *>(&a), sizeof(a));
  if (result == SOCKET_ERROR)
    SocketError();
  else {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1) < 4)
      REXLOG_INFO("FM2 LAN UDP sent {} bytes to {}:{}", result,
                  IpText(IpBytes(ntohl(a.sin_addr.s_addr))), ntohs(a.sin_port));
  }
  return uint32_t(result);
}
u32 ReceiveFrom(u32, u32 handle, mapped_void data, u32 size, u32 flags,
                ppc_ptr_t<rex::system::XSOCKADDR_IN> from, mapped_u32 len) {
  if (!data || (from && (!len || uint32_t(*len) < 16))) {
    WSASetLastError(WSAEINVAL);
    SocketError();
    return uint32_t(-1);
  }
  SOCKET socket = Native(handle);
  if (socket == INVALID_SOCKET)
    return uint32_t(-1);
  if (RelayActive()) {
    Ip source{};
    uint16_t port = 0;
    const int received =
        RelayReceiveFrom(handle, socket, data.host_address(), int(size),
                         int(flags), source, port);
    if (received == SOCKET_ERROR) {
      SocketError();
      return uint32_t(-1);
    }
    if (from) {
      *from = {};
      from->sin_family = AF_INET;
      from->sin_port = port;
      from->sin_addr = IpValue(source);
      *len = 16;
    }
    return uint32_t(received);
  }
  sockaddr_in a{};
  int n = sizeof(a);
  int result =
      recvfrom(socket, reinterpret_cast<char *>(data.host_address()), int(size),
               int(flags), reinterpret_cast<sockaddr *>(&a), &n);
  if (result == SOCKET_ERROR) {
    SocketError();
    return uint32_t(-1);
  }
  {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1) < 4)
      REXLOG_INFO("FM2 LAN UDP received {} bytes from {}:{}", result,
                  IpText(IpBytes(ntohl(a.sin_addr.s_addr))), ntohs(a.sin_port));
  }
  if (from) {
    *from = {};
    from->sin_family = a.sin_family;
    from->sin_port = ntohs(a.sin_port);
    from->sin_addr = ntohl(a.sin_addr.s_addr);
    *len = 16;
  }
  return uint32_t(result);
}
#include "lan_wsa.inc"
using GuestFunction = void (*)(PPCContext &, uint8_t *);
void Original(const char *name, PPCContext &ctx, uint8_t *base) {
  static std::mutex lock;
  static std::unordered_map<std::string, GuestFunction> functions;
  GuestFunction fn;
  {
    std::lock_guard guard(lock);
    auto [it, inserted] = functions.try_emplace(name, nullptr);
    if (inserted)
      it->second = reinterpret_cast<GuestFunction>(
          GetProcAddress(GetModuleHandleW(L"rexruntime.dll"), name));
    fn = it->second;
  }
  if (fn)
    fn(ctx, base);
  else {
    REXLOG_ERROR("FM2 LAN could not resolve original {}", name);
    ctx.r3.u64 = 50;
  }
}
} // namespace
void Shutdown() {
  wsa_queue.Shutdown();
  RelayStop();
}
bool SelfTestRequested() { return REXCVAR_GET(fm2_lan_selftest); }
bool RunTransportSelfTest() {
  if (!REXCVAR_GET(fm2_lan) || !PeerIp())
    return false;
  WSADATA startup{};
  if (WSAStartup(MAKEWORD(2, 2), &startup) != 0)
    return false;
  auto *object = new XSocket(REX_KERNEL_STATE());
  if (object->Initialize(XSocket::X_AF_INET, XSocket::X_SOCK_DGRAM,
                         XSocket::X_IPPROTO_UDP) != 0) {
    object->ReleaseHandle();
    WSACleanup();
    return false;
  }
  const auto handle = object->handle();
  bool passed = false;
  rex::system::XSOCKADDR_IN bind_address{};
  bind_address.sin_family = AF_INET;
  bind_address.sin_port = 39741;
  auto bp = ppc_ptr_t<rex::system::XSOCKADDR_IN>::from_host(&bind_address);
  if (Bind(1, handle, bp, 16) == 0) {
    rex::be<uint32_t> length = 16;
    rex::system::XSOCKADDR_IN bound{};
    const auto boundp = ppc_ptr_t<rex::system::XSOCKADDR_IN>::from_host(&bound);
    const auto lengthp = mapped_u32::from_host(&length);
    bool queries_ok = GetSockName(1, handle, boundp, lengthp) == 0 &&
                      uint32_t(bound.sin_addr) == IpValue(LocalIp()) &&
                      uint16_t(bound.sin_port) == 39741;
    rex::be<uint32_t> option = 0;
    length = 4;
    queries_ok = queries_ok &&
                 GetSockOpt(1, handle, SOL_SOCKET, SO_TYPE,
                            mapped_void::from_host(&option), lengthp) == 0 &&
                 uint32_t(option) == SOCK_DGRAM;
    struct Packet {
      std::array<uint8_t, 8> magic{'F', 'M', '2', 'L', 'A', 'N', '0', '1'};
      uint8_t kind = 1;
      Address address{};
      KeyId id{};
      std::array<uint8_t, 16> key{};
    };
    Packet outgoing{};
    outgoing.address = LocalAddress();
    CreateKey(1, mapped_void::from_host(outgoing.id.data()),
              mapped_void::from_host(outgoing.key.data()));
    rex::system::XSOCKADDR_IN destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = 39741;
    destination.sin_addr = 0xffffffff;
    const auto dp =
        ppc_ptr_t<rex::system::XSOCKADDR_IN>::from_host(&destination);
    const auto start = std::chrono::steady_clock::now();
    auto last_send = start - std::chrono::seconds(1);
    bool hello = false, ack = false;
    while (queries_ok && std::chrono::steady_clock::now() - start <
                             std::chrono::seconds(10)) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_send >= std::chrono::milliseconds(100)) {
        outgoing.kind = 1;
        SendTo(1, handle, mapped_void::from_host(&outgoing), sizeof(outgoing),
               0, dp, 16);
        last_send = now;
      }
      fd_set read;
      FD_ZERO(&read);
      FD_SET(SOCKET(object->native_handle()), &read);
      timeval timeout{0, 50000};
      if (select(0, &read, nullptr, nullptr, &timeout) <= 0)
        continue;
      Packet incoming{};
      rex::system::XSOCKADDR_IN source{};
      length = 16;
      if (ReceiveFrom(1, handle, mapped_void::from_host(&incoming),
                      sizeof(incoming), 0,
                      ppc_ptr_t<rex::system::XSOCKADDR_IN>::from_host(&source),
                      lengthp) != sizeof(incoming))
        continue;
      if (incoming.magic != outgoing.magic ||
          incoming.address.local != *PeerIp())
        continue;
      RegisterKey(1, mapped_void::from_host(incoming.id.data()),
                  mapped_void::from_host(incoming.key.data()));
      Ip resolved{};
      if (ToIp(1, ppc_ptr_t<Address>::from_host(&incoming.address),
               mapped_void::from_host(incoming.id.data()),
               mapped_void::from_host(resolved.data())) != 0 ||
          resolved != *PeerIp())
        break;
      if (incoming.kind == 1) {
        hello = true;
        outgoing.kind = 2;
        SendTo(1, handle, mapped_void::from_host(&outgoing), sizeof(outgoing),
               0, dp, 16);
      } else if (incoming.kind == 2)
        ack = true;
      if (hello && ack) {
        passed = true;
        break;
      }
    }
  }
  object->Close();
  object->ReleaseHandle();
  WSACleanup();
  if (passed)
    REXLOG_WARN("FM2 LAN TRANSPORT SELFTEST PASSED: peer {}, socket queries, "
                "discovery, key registration and reply",
                IpText(*PeerIp()));
  else
    REXLOG_ERROR(
        "FM2 LAN TRANSPORT SELFTEST FAILED (no complete peer handshake)");
  return passed;
}
void ConfigureIdentity() {
  if (!REXCVAR_GET(fm2_lan))
    return;
  const auto xuid = OfflineXuid(LocalAddress());
  REX_KERNEL_STATE()->user_profile()->set_xuid(xuid);
  REXLOG_INFO("FM2 LAN local player identity {:016X}", xuid);
}
// First-chance handler: a guest null-page access (as seen in multiplayer
// crashes) gets the guest LR and back chain logged before the SDK's crash
// report, which only has unsymbolised host frames. Other faults (MMIO, watched
// pages) are left untouched.
std::string GuestBacktrace(PPCContext &ctx, uint8_t *base);
LONG CALLBACK GuestNullAccess(EXCEPTION_POINTERS *info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      info->ExceptionRecord->NumberParameters < 2)
    return EXCEPTION_CONTINUE_SEARCH;
  auto *base = static_cast<uint8_t *>(REX_KERNEL_MEMORY()->TranslateVirtual(0));
  const auto host = info->ExceptionRecord->ExceptionInformation[1];
  const auto *state = rex::runtime::ThreadState::Get();
  if (host < uintptr_t(base) || host - uintptr_t(base) >= 0x10000 || !state)
    return EXCEPTION_CONTINUE_SEARCH;
  static std::atomic<unsigned> logged{0};
  if (logged.fetch_add(1) < 4) {
    auto &ctx = *state->context();
    REXLOG_ERROR("FM2 LAN guest null access 0x{:X}: r3={:08X} r31={:08X} "
                 "stack {}",
                 host - uintptr_t(base), ctx.r3.u32, ctx.r31.u32,
                 GuestBacktrace(ctx, base));
    rex::FlushLogging();
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
void ValidateConfiguration() {
  if (!REXCVAR_GET(fm2_relay).empty() || REXCVAR_GET(fm2_lan))
    AddVectoredExceptionHandler(1, GuestNullAccess);
  const std::string target = REXCVAR_GET(fm2_relay);
  // An explicit --fm2_lan_ip selects a direct pair even though the launcher
  // always writes fm2_relay to forza2.user.toml.
  if (!target.empty() && REXCVAR_GET(fm2_lan_ip).empty()) {
    // Online play implies the LAN hooks; if the server is unreachable, stay
    // offline instead of failing later inside game code.
    // Everyone shares one room: all sessions on the server are visible.
    const bool joined = RelayStart(target, "public");
    rex::cvar::SetFlagByName("fm2_lan", joined ? "true" : "false");
    if (!joined)
      REXLOG_ERROR("FM2 relay unavailable; System Link stays offline");
    return;
  }
  if (!REXCVAR_GET(fm2_lan))
    return;
  REXLOG_WARN("FM2 LAN EXPERIMENTAL: local {} port {} peer {}; "
              "session/discovery integration is not yet validated",
              IpText(LocalIp()), REXCVAR_GET(fm2_lan_port),
              PeerIp() ? IpText(*PeerIp()) : "broadcast");
}
// Walks the guest PPC back chain; callers store LR at (frame - 8) in this title.
std::string GuestBacktrace(PPCContext &ctx, uint8_t *base) {
  auto load = [base](uint32_t address) {
    uint32_t value;
    std::memcpy(&value, base + address, 4);
    return _byteswap_ulong(value);
  };
  std::string text = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t frame = ctx.r1.u32;
  // Heap ranges include reserved-but-uncommitted pages (e.g. past the stack
  // top), so check the host page is actually committed before reading.
  auto mapped = [base](uint32_t address) {
    if (address < 8 || address > 0xFFFFFFF0u)
      return false;
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(base + address, &info, sizeof(info)) &&
           info.State == MEM_COMMIT &&
           !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           base + address + 4 <=
               static_cast<uint8_t *>(info.BaseAddress) + info.RegionSize;
  };
  for (int depth = 0; depth < 16 && mapped(frame); ++depth) {
    const uint32_t next = load(frame);
    if (next <= frame || !mapped(next) || !mapped(next - 8))
      break;
    text += fmt::format(" {:08X}", load(next - 8));
    frame = next;
  }
  return text;
}
} // namespace forza2::lan
#define FM2_LAN_HOOK(symbol, fn)                                             \
  extern "C" void symbol(PPCContext &ctx, uint8_t *base) {                     \
    if (!REXCVAR_GET(fm2_lan)) {                                               \
      forza2::lan::Original(#symbol, ctx, base);                               \
      return;                                                                  \
    }                                                                          \
    forza2::lan::guest_lr = uint32_t(ctx.lr);                                  \
    rex::ppc::HostToGuestFunction<forza2::lan::fn>(ctx, base);                 \
  }
FM2_LAN_HOOK(__imp__NetDll_XNetGetEthernetLinkStatus, Link)
FM2_LAN_HOOK(__imp__NetDll_XNetGetTitleXnAddr, TitleAddress)
FM2_LAN_HOOK(__imp__NetDll_XNetCreateKey, CreateKey)
FM2_LAN_HOOK(__imp__NetDll_XNetRandom, Random)
FM2_LAN_HOOK(__imp__NetDll_XNetRegisterKey, RegisterKey)
FM2_LAN_HOOK(__imp__NetDll_XNetUnregisterKey, UnregisterKey)
FM2_LAN_HOOK(__imp__NetDll_XNetXnAddrToInAddr, ToIp)
FM2_LAN_HOOK(__imp__NetDll_XNetInAddrToXnAddr, FromIp)
FM2_LAN_HOOK(__imp__NetDll_XNetConnect, Connect)
FM2_LAN_HOOK(__imp__NetDll_XNetGetConnectStatus, ConnectStatus)
FM2_LAN_HOOK(__imp__NetDll_XNetUnregisterInAddr, UnregisterIp)
FM2_LAN_HOOK(__imp__NetDll_XNetInAddrToString, AddressString)
FM2_LAN_HOOK(__imp__NetDll_bind, Bind)
FM2_LAN_HOOK(__imp__NetDll_getsockname, GetSockName)
FM2_LAN_HOOK(__imp__NetDll_getsockopt, GetSockOpt)
FM2_LAN_HOOK(__imp__NetDll_sendto, SendTo)
FM2_LAN_HOOK(__imp__NetDll_recvfrom, ReceiveFrom)
FM2_LAN_HOOK(__imp__NetDll_WSASendTo, WsaSendTo)
FM2_LAN_HOOK(__imp__NetDll_WSASend, WsaSend)
FM2_LAN_HOOK(__imp__NetDll_WSARecv, WsaReceive)
FM2_LAN_HOOK(__imp__NetDll_connect, SocketConnect)
FM2_LAN_HOOK(__imp__NetDll_send, SocketSend)
FM2_LAN_HOOK(__imp__NetDll_recv, SocketReceive)
FM2_LAN_HOOK(__imp__NetDll_WSARecvFrom, WsaReceiveFrom)
FM2_LAN_HOOK(__imp__NetDll_WSAGetOverlappedResult, WsaGetResult)
FM2_LAN_HOOK(__imp__NetDll_closesocket, CloseSocket)
// Unit tests link lan.cpp without the recompiled title code.
#ifndef FM2_LAN_NO_GUEST_WRAPPERS
// DirectPlay-style command completion: a nonzero HRESULT at +80 tears down the
// session. Log it to find which local step rejects a System Link join.
extern "C" void __imp__sub_826D1288(PPCContext &__restrict ctx, uint8_t *base);
extern "C" void sub_826D1288(PPCContext &__restrict ctx, uint8_t *base) {
  if (REXCVAR_GET(fm2_lan)) {
    auto load = [base](uint32_t address) {
      uint32_t value;
      std::memcpy(&value, base + address, 4);
      return _byteswap_ulong(value);
    };
    const uint32_t command = ctx.r4.u32;
    const uint32_t result = load(command + 80);
    if (result)
      REXLOG_WARN("FM2 LAN command {:08X} failed hr={:08X} type={:08X} "
                  "flags={:08X} parent={:08X} stack {}",
                  command, result, load(command), load(command + 12),
                  load(command + 48), forza2::lan::GuestBacktrace(ctx, base));
  }
  __imp__sub_826D1288(ctx, base);
}
// DirectPlay endpoint creation (sub_826C93C8). Normally the send window starts
// small and every retry timeout counts as a drop (sub_826C98E0) that halves it
// (sub_826C9200). Over the relay's ~25 ms round trip that pins the window at
// one frame: a strict send/ack ping-pong of ~6 updates per second with
// half-second stalls, seen as stuttering remote cars. Online, apply the
// title's own "throttling disabled" branch: a 63-frame window and EPD flag
// 0x400, which makes the drop counter a no-op.
extern "C" void __imp__sub_826C93C8(PPCContext &__restrict ctx, uint8_t *base);
extern "C" void sub_826C93C8(PPCContext &__restrict ctx, uint8_t *base) {
  const uint32_t protocol = ctx.r3.u32;
  __imp__sub_826C93C8(ctx, base);
  const uint32_t endpoint = ctx.r3.u32;
  if (!REXCVAR_GET(fm2_lan) || !forza2::lan::RelayActive() || !endpoint)
    return;
  auto load = [base](uint32_t address) {
    uint32_t value;
    std::memcpy(&value, base + address, 4);
    return _byteswap_ulong(value);
  };
  auto store = [base](uint32_t address, uint32_t value) {
    value = _byteswap_ulong(value);
    std::memcpy(base + address, &value, 4);
  };
  constexpr uint32_t kWindowFrames = 63, kNoThrottle = 0x400;
  const uint32_t flags = load(endpoint + 20);
  if (flags & kNoThrottle)
    return;
  const uint32_t max_frame = load(protocol + 20); // as the native branch
  store(endpoint + 248, kWindowFrames);             // send window (frames)
  store(endpoint + 256, kWindowFrames);             // window floor
  store(endpoint + 252, kWindowFrames * max_frame); // send window (bytes)
  store(endpoint + 20, flags | kNoThrottle);
  static std::atomic<unsigned> logged{0};
  if (logged.fetch_add(1) < 16)
    REXLOG_INFO("FM2 relay endpoint {:08X}: throttling off, window {} frames "
                "x {} bytes",
                endpoint, kWindowFrames, max_frame);
}
// The Connect path in sub_826E79F0 reports E_OUTOFMEMORY when one of these
// returns null. Log their arguments and results while diagnosing joins.
#define FM2_LAN_WRAP(name)                                                     \
  extern "C" void __imp__##name(PPCContext &__restrict ctx, uint8_t *base);    \
  extern "C" void name(PPCContext &__restrict ctx, uint8_t *base) {            \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32,          \
                   r6 = ctx.r6.u32, lr = uint32_t(ctx.lr);                     \
    __imp__##name(ctx, base);                                                  \
    static std::atomic<unsigned> count{0};                                     \
    if (REXCVAR_GET(fm2_lan) && count.fetch_add(1) < 64)                       \
      REXLOG_INFO("FM2 LAN " #name "({:08X},{:08X},{:08X},{:08X}) = {:08X} "   \
                  "lr={:08X}",                                                 \
                  r3, r4, r5, r6, ctx.r3.u32, lr);                             \
  }
FM2_LAN_WRAP(sub_826DD9D8)
FM2_LAN_WRAP(sub_826E9C70)
FM2_LAN_WRAP(sub_826E5690)
#endif
// Observe the remaining runtime stubs only in the opt-in LAN lab.
#define FM2_LAN_TRACE(symbol)                                                  \
  extern "C" void symbol(PPCContext &ctx, uint8_t *base) {                     \
    if (REXCVAR_GET(fm2_lan)) {                                                \
      static std::atomic<unsigned> count{0};                                   \
      if ((ctx.r3.u32 == 0xFB || std::strstr(#symbol, "XMsg") == nullptr) &&   \
          count.fetch_add(1) < 24)                                             \
        REXLOG_INFO("FM2 LAN runtime {} r3={:08X} r4={:08X} lr={:08X}",        \
                    #symbol, ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));        \
      if (std::strstr(#symbol, "shutdown"))                                    \
        REXLOG_INFO("FM2 LAN guest stack {}",                                  \
                    forza2::lan::GuestBacktrace(ctx, base));                   \
    }                                                                          \
    forza2::lan::Original(#symbol, ctx, base);                                 \
  }
FM2_LAN_TRACE(__imp__XMsgInProcessCall)
FM2_LAN_TRACE(__imp__XMsgStartIORequest)
FM2_LAN_TRACE(__imp__XMsgStartIORequestEx)
FM2_LAN_TRACE(__imp__XamSessionCreateHandle)
FM2_LAN_TRACE(__imp__NetDll_XNetQosListen)
FM2_LAN_TRACE(__imp__NetDll_XNetQosLookup)
FM2_LAN_TRACE(__imp__NetDll_socket)
FM2_LAN_TRACE(__imp__NetDll_setsockopt)
FM2_LAN_TRACE(__imp__NetDll_ioctlsocket)
FM2_LAN_TRACE(__imp__NetDll_select)
FM2_LAN_TRACE(__imp__NetDll_shutdown)
FM2_LAN_TRACE(__imp__NetDll_XNetServerToInAddr)
FM2_LAN_TRACE(__imp__NetDll_XNetQosRelease)
#else
#include "lan.h"
namespace forza2::lan {
void ValidateConfiguration() {}
void ConfigureIdentity() {}
void Shutdown() {}
bool SelfTestRequested() { return false; }
bool RunTransportSelfTest() { return false; }
} // namespace forza2::lan
#endif
