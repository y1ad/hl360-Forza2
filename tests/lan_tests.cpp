#ifdef NDEBUG
#error LAN tests require assertions enabled
#endif
#define FM2_LAN_NO_GUEST_WRAPPERS
#include "../src/lan.cpp"
#include <cassert>
#include <iostream>

in_addr IpNative(const char *text) {
  in_addr ip{};
  assert(InetPtonA(AF_INET, text, &ip) == 1);
  return ip;
}

int main() {
  using namespace forza2::lan;
  auto ip = ParseIp("127.0.0.2");
  assert(ip && IpValue(*ip) == 0x7f000002);
  for (auto bad :
       {"1.2.3", "1.2.3.256", "1.2.3.4x", "-1.2.3.4", "1..2.3", "1.2.3.4.5"})
    assert(!ParseIp(bad));
  rex::cvar::SetFlagByName("fm2_lan_ip", "127.0.0.2");
  rex::cvar::SetFlagByName("fm2_lan_peer", "127.0.0.3");
  rex::cvar::SetFlagByName("fm2_lan", "false");
  PPCContext ctx{};
  __imp__NetDll_XNetGetEthernetLinkStatus(ctx, nullptr);
  assert(ctx.r3.u32 == 0);
  rex::cvar::SetFlagByName("fm2_lan", "true");
  __imp__NetDll_XNetGetEthernetLinkStatus(ctx, nullptr);
  assert(ctx.r3.u32 == 0x0b);
  assert(Connect(1, 0xffffffffu) == 0 && ConnectStatus(1, 0xffffffffu) == 2);
  Address address{};
  assert(TitleAddress(1, ppc_ptr_t<Address>::from_host(&address)) == 0x0c);
  assert(address.local == *ip && address.online == Ip{} && address.mac[0] == 2);
  assert(OfflineXuid(address) !=
         OfflineXuid(MakeAddress(*ParseIp("127.0.0.3"), 2302)));
  KeyId id{}, second{};
  std::array<uint8_t, 16> exchange{}, other{};
  auto idp = mapped_void::from_host(id.data()),
       kp = mapped_void::from_host(exchange.data());
  assert(CreateKey(1, idp, kp) == 0 &&
         CreateKey(1, mapped_void::from_host(second.data()),
                   mapped_void::from_host(other.data())) == 0);
  assert(id != second && exchange != other && (id[0] & 0xf0) == 0);
  auto remote = MakeAddress(*ParseIp("127.0.0.3"), 3074);
  Ip output{};
  assert(ToIp(1, ppc_ptr_t<Address>::from_host(&remote), idp,
              mapped_void::from_host(output.data())) == 10022);
  assert(RegisterKey(1, idp, kp) == 0);
  Address incoming{};
  KeyId incoming_id{};
  assert(FromIp(1, 0x7f000003, ppc_ptr_t<Address>::from_host(&incoming),
                mapped_void::from_host(incoming_id.data())) == 0);
  assert(incoming.local == remote.local && incoming_id == id);
  assert(FromIp(1, 0x7f000004, ppc_ptr_t<Address>::from_host(&incoming),
                mapped_void::from_host(incoming_id.data())) == 10022);
  assert(UnregisterIp(1, 0x7f000003) == 0);
  assert(RegisterKey(1, mapped_void::from_host(second.data()),
                     mapped_void::from_host(other.data())) == 0);
  assert(FromIp(1, 0x7f000003, ppc_ptr_t<Address>::from_host(&incoming),
                mapped_void::from_host(incoming_id.data())) == 10022);
  assert(UnregisterKey(1, mapped_void::from_host(second.data())) == 0);
  assert(ToIp(1, ppc_ptr_t<Address>::from_host(&remote), idp,
              mapped_void::from_host(output.data())) == 0 &&
         output == remote.local);
  Address recovered{};
  KeyId recovered_id{};
  assert(FromIp(1, IpValue(remote.local),
                ppc_ptr_t<Address>::from_host(&recovered),
                mapped_void::from_host(recovered_id.data())) == 0);
  assert(recovered.local == remote.local && recovered_id == id);
  // FM2's DirectPlay URL builder passes a null key ID output.
  recovered = {};
  assert(FromIp(1, IpValue(remote.local),
                ppc_ptr_t<Address>::from_host(&recovered), {}) == 0 &&
         recovered.local == remote.local);
  assert(UnregisterKey(1, idp) == 0 &&
         ConnectStatus(1, IpValue(remote.local)) == 0);
  assert(ToIp(1, {}, idp, {}) == 10022);

  // Two native UDP sockets exercise guest sockaddr conversion and direct
  // discovery.
  WSADATA startup{};
  assert(WSAStartup(MAKEWORD(2, 2), &startup) == 0);
  SOCKET a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP),
         b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  assert(a != INVALID_SOCKET && b != INVALID_SOCKET);
  rex::system::XSOCKADDR_IN guest{};
  guest.sin_family = AF_INET;
  guest.sin_addr = 0;
  auto local = NativeAddress(guest, true);
  assert(bind(a, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0);
  local.sin_addr.s_addr = IpNative("127.0.0.3").s_addr;
  assert(bind(b, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0);
  int length = sizeof(local);
  assert(getsockname(b, reinterpret_cast<sockaddr *>(&local), &length) == 0);
  DWORD timeout = 1000;
  setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout),
             sizeof(timeout));
  setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout),
             sizeof(timeout));
  guest.sin_addr = 0xffffffff;
  guest.sin_port = ntohs(local.sin_port);
  auto dest = NativeAddress(guest, false);
  assert(dest.sin_addr.s_addr == IpNative("127.0.0.3").s_addr &&
         dest.sin_port == local.sin_port);
  const char ping[] = "FM2-LAN-discovery";
  assert(sendto(a, ping, sizeof(ping), 0, reinterpret_cast<sockaddr *>(&dest),
                sizeof(dest)) == sizeof(ping));
  char buffer[64]{};
  sockaddr_in source{};
  length = sizeof(source);
  assert(recvfrom(b, buffer, sizeof(buffer), 0,
                  reinterpret_cast<sockaddr *>(&source),
                  &length) == sizeof(ping));
  assert(std::strcmp(buffer, ping) == 0 &&
         source.sin_addr.s_addr == IpNative("127.0.0.2").s_addr);
  assert(sendto(b, buffer, sizeof(ping), 0,
                reinterpret_cast<sockaddr *>(&source), length) == sizeof(ping));
  assert(recv(a, buffer, sizeof(buffer), 0) == sizeof(ping));
  // The real async receive path must stay pending, publish scatter/gather
  // outputs, and support reuse and cancellation without a kernel/guest thread.
  LanWsaOverlapped overlap{};
  rex::be<uint32_t> transferred = 0, received_flags = 0, source_size = 16;
  rex::system::XSOCKADDR_IN guest_source{};
  char first[4]{}, rest[64]{};
  rex::be<uint32_t> async_bytes = 0xdeadbeef, async_flags = 0xcafebabe;
  auto pending = std::make_unique<LanWsaOperation>();
  pending->socket = b;
  pending->handle = 42;
  pending->receive = true;
  pending->guest = ppc_ptr_t<LanWsaOverlapped>::from_host(&overlap);
  pending->byte_output = mapped_u32::from_host(&async_bytes);
  pending->flag_output = mapped_u32::from_host(&async_flags);
  pending->source =
      ppc_ptr_t<rex::system::XSOCKADDR_IN>::from_host(&guest_source);
  pending->source_size = mapped_u32::from_host(&source_size);
  pending->buffers = {{sizeof(first), first}, {sizeof(rest), rest}};
  assert(wsa_queue.Submit(std::move(pending)) == WSA_IO_PENDING);
  assert(uint32_t(overlap.status) == 0x103);
  assert(wsa_queue.Result(ppc_ptr_t<LanWsaOverlapped>::from_host(&overlap),
                          mapped_u32::from_host(&transferred),
                          mapped_u32::from_host(&received_flags)) ==
         WSA_IO_INCOMPLETE);
  auto outgoing = std::make_unique<LanWsaOperation>();
  outgoing->socket = a;
  outgoing->address = dest;
  outgoing->buffers = {{4, const_cast<char *>(ping)},
                       {sizeof(ping) - 4, const_cast<char *>(ping + 4)}};
  assert(wsa_queue.Submit(std::move(outgoing)) == 0);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  uint32_t result;
  do {
    result = wsa_queue.Result(ppc_ptr_t<LanWsaOverlapped>::from_host(&overlap),
                              mapped_u32::from_host(&transferred),
                              mapped_u32::from_host(&received_flags));
    if (result != WSA_IO_INCOMPLETE)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  assert(result == 0 && uint32_t(transferred) == sizeof(ping));
  assert(uint32_t(async_bytes) == 0xdeadbeef &&
         uint32_t(async_flags) == 0xcafebabe);
  assert(std::memcmp(first, ping, 4) == 0 && std::strcmp(rest, ping + 4) == 0);
  assert(uint32_t(guest_source.sin_addr) == 0x7f000002 &&
         uint32_t(source_size) == 16);
  // Reuse the same OVERLAPPED only after completion, then cancel it.
  pending = std::make_unique<LanWsaOperation>();
  pending->socket = b;
  pending->handle = 42;
  pending->receive = true;
  pending->guest = ppc_ptr_t<LanWsaOverlapped>::from_host(&overlap);
  pending->buffers = {{sizeof(rest), rest}};
  assert(wsa_queue.Submit(std::move(pending)) == WSA_IO_PENDING);
  assert(uint32_t(overlap.status) == 0x103);
  wsa_queue.Cancel(42);
  assert(uint32_t(overlap.status) == (0x80070000u | WSA_OPERATION_ABORTED));
  assert(wsa_queue.Result(ppc_ptr_t<LanWsaOverlapped>::from_host(&overlap),
                          mapped_u32::from_host(&transferred),
                          mapped_u32::from_host(&received_flags)) ==
         WSA_OPERATION_ABORTED);
  // Connected UDP is the path used after discovery; exercise both native
  // WSASend and WSARecv, including asynchronous completion.
  sockaddr_in a_address{};
  int a_length = sizeof(a_address);
  assert(getsockname(a, reinterpret_cast<sockaddr *>(&a_address), &a_length) ==
         0);
  assert(connect(a, reinterpret_cast<sockaddr *>(&dest), sizeof(dest)) == 0);
  assert(connect(b, reinterpret_cast<sockaddr *>(&a_address),
                 sizeof(a_address)) == 0);
  std::memset(rest, 0, sizeof(rest));
  pending = std::make_unique<LanWsaOperation>();
  pending->socket = b;
  pending->handle = 42;
  pending->receive = true;
  pending->connected = true;
  pending->guest = ppc_ptr_t<LanWsaOverlapped>::from_host(&overlap);
  pending->buffers = {{sizeof(rest), rest}};
  assert(wsa_queue.Submit(std::move(pending)) == WSA_IO_PENDING);
  outgoing = std::make_unique<LanWsaOperation>();
  outgoing->socket = a;
  outgoing->connected = true;
  outgoing->buffers = {{4, const_cast<char *>(ping)},
                       {sizeof(ping) - 4, const_cast<char *>(ping + 4)}};
  assert(wsa_queue.Submit(std::move(outgoing)) == 0);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    result = wsa_queue.Result(ppc_ptr_t<LanWsaOverlapped>::from_host(&overlap),
                              mapped_u32::from_host(&transferred),
                              mapped_u32::from_host(&received_flags));
    if (result != WSA_IO_INCOMPLETE)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  assert(result == 0 && uint32_t(transferred) == sizeof(ping) &&
         std::strcmp(rest, ping) == 0);

  wsa_queue.Shutdown();

  // Relay loss recovery: datagram 3 of 5 is lost on the way, yet the receiver
  // delivers 1..5 exactly once and in order, and ignores a repeated packet.
  relay.server.sin_family = AF_INET;
  relay.server.sin_port = htons(23020);
  relay.server.sin_addr.s_addr = htonl(0x7f000001);
  relay.redundant = true;
  const Ip peer_ip{10, 68, 0, 9};
  std::vector<std::vector<uint8_t>> wire;
  for (uint8_t i = 1; i <= 5; ++i) {
    const uint8_t payload[3] = {i, i, i};
    auto packet = RelayEncode(7, peer_ip, 2302, payload, sizeof(payload));
    assert(packet[1] == kRelayDataOutRedundant);
    packet[1] = kRelayDataInRedundant; // what the server forwards
    wire.push_back(packet);
  }
  std::vector<uint8_t> delivered;
  for (size_t i : {0, 1, 3, 4, 4}) {
    for (const auto &d : RelayDecode(8, wire[i].data(), int(wire[i].size()),
                                     relay.server)) {
      assert(d.payload.size() == 3 && d.from == peer_ip && d.port == 2302);
      delivered.push_back(d.payload[0]);
    }
  }
  assert((delivered == std::vector<uint8_t>{1, 2, 3, 4, 5}));
  const uint8_t broadcast_payload[1] = {9};
  assert(RelayEncode(7, Ip{255, 255, 255, 255}, 2302, broadcast_payload, 1)[1] ==
         kRelayDataOut);
  relay.redundant = false;

  closesocket(a);
  closesocket(b);
  WSACleanup();
  std::cout << "PASS: offline delegation, LAN identity, keys, address "
               "round-trip, UDP discovery, connected UDP, async scatter/gather, "
               "cancellation and relay loss recovery\n";
}
