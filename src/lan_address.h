#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace forza2::lan {
using Ip = std::array<uint8_t, 4>;
using KeyId = std::array<uint8_t, 8>;
struct Address {
  Ip local{};
  Ip online{};
  std::array<uint8_t, 2> port{};
  std::array<uint8_t, 6> mac{};
  std::array<uint8_t, 20> online_id{};
};
static_assert(sizeof(Address) == 36);
inline std::optional<Ip> ParseIp(std::string_view text) {
  Ip result{};
  size_t pos = 0;
  for (size_t i = 0; i < 4; ++i) {
    unsigned value = 0, digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      value = value * 10 + unsigned(text[pos++] - '0');
      if (++digits > 3 || value > 255)
        return {};
    }
    if (!digits)
      return {};
    result[i] = uint8_t(value);
    if (i != 3 && (pos == text.size() || text[pos++] != '.'))
      return {};
  }
  return pos == text.size() ? std::optional<Ip>(result) : std::nullopt;
}
inline uint32_t IpValue(const Ip &ip) {
  return uint32_t(ip[0]) << 24 | uint32_t(ip[1]) << 16 | uint32_t(ip[2]) << 8 |
         ip[3];
}
inline Ip IpBytes(uint32_t value) {
  return {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8),
          uint8_t(value)};
}
inline std::string IpText(const Ip &ip) {
  return std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." +
         std::to_string(ip[2]) + "." + std::to_string(ip[3]);
}
inline bool IsUsable(const Ip &ip) {
  return ip[0] != 0 && ip[0] < 224 && IpValue(ip) != 0xffffffffu;
}
inline Address MakeAddress(const Ip &ip, uint16_t port) {
  Address a{};
  a.local = ip;
  a.port = {uint8_t(port >> 8), uint8_t(port)};
  // Locally administered unicast identity, stable for this IP/port pair.
  a.mac = {0x02,  uint8_t(port >> 8),   ip[0], ip[1],
           ip[2], uint8_t(ip[3] ^ port)};
  return a;
}
inline uint64_t OfflineXuid(const Address &address) {
  const uint16_t port = uint16_t(address.port[0]) << 8 | address.port[1];
  return UINT64_C(0xE000000000000000) | (uint64_t(port) << 32) |
         IpValue(address.local);
}
inline Ip Destination(const Ip &requested, const std::optional<Ip> &peer) {
  return IpValue(requested) == 0xffffffffu && peer ? *peer : requested;
}
} // namespace forza2::lan
