# FM2 LAN prototype

Experimental. Two local instances (127.0.0.2/127.0.0.3) have discovered,
joined and raced each other through the native System Link menus. Two
physical PCs and Internet play have not been validated yet.
The normal launcher remains offline. No VPS services have been installed.

## Start two local instances

From a PowerShell terminal in this project:

    powershell -ExecutionPolicy Bypass -File scripts/lan-lab.ps1 -Role host
    powershell -ExecutionPolicy Bypass -File scripts/lan-lab.ps1 -Role guest

The lab uses 127.0.0.2 and 127.0.0.3, both on UDP 2302, with independent
user data, cache and logs under out/lan-lab/host and out/lan-lab/guest.
Game content defaults to assets; pass -GameDataRoot when imported elsewhere.
These role names select addresses/data directories, not an automatic in-game
host/join operation. Select the relevant System Link options in each game.

For two PCs, pass -LocalIp <this PC's LAN IPv4> -PeerIp <other PC's LAN IPv4>
on each computer. Both clients need the same game revision/mods. The adapter
uses plain UDP between compatible port clients; Xbox 360 console compatibility
and Xbox Live are not implemented. No firewall settings are changed by the script.

## Implemented

* Opt-in --fm2_lan=true; disabled calls delegate to the original runtime exports.
* Explicit local IPv4, independent locally administered MAC and offline XUID.
* Session key generation, registration and address/identity round-trips.
  For a configured direct pair, incoming addresses can be associated with the
  sole registered session key; arbitrary sources and ambiguous keys are rejected.
* UDP bind/send/receive and socket queries with network-order IP/port handling.
* WSA scatter/gather sends and receives, both addressed and connected
  (WSASendTo/WSARecvFrom and WSASend/WSARecv), including overlapped receives, guest
  event notification, result queries and cancellation before socket close.
  Completion-routine callbacks are explicitly unsupported; event completion is implemented.
* Optional directed discovery: guest broadcast destinations go to --fm2_lan_peer.
* Bounded first-packet metadata logs; packet contents are not logged.

The game has been observed binding UDP 2302. Missing getsockopt/getsockname calls
found during the two-process boot were implemented. Keys are bookkeeping for
plain-UDP peers; they do not implement Xbox peer encryption or a verified
connection handshake. XNet connection status means the peer route is registered.

## Tests and remaining work

Build and run fm2-lan-tests from the shared Vulkan CMake build. Tests exercise
offline delegation, IP validation, distinct identities, session key validation,
address round-trips and a real UDP request/reply between loopback peers.
They also test connected UDP send/receive, a pending native overlapped receive, scatter/gather payloads,
source address/byte count publication, OVERLAPPED reuse and cancellation.

Successful native UDP transport is not proof of working game matchmaking.
XGI session creation/search/join still contain placeholder implementations in
the SDK (fake session handles and no populated search results). QoS also needs
a real implementation or a verified LAN-specific path. Finish these parts and
validate lobby discovery, joining and race synchronization before advertising
multiplayer as usable. The VPS lobby/directory/relay is a later stage.

A real FM2 instance reached its System Link meeting point and was observed
using WSASendTo for 21-byte discovery datagrams on UDP 2302. The SDK receive
stub also had the wrong ABI (missing fromlen), which the port override fixes.
After fixing the Xbox pending status (0x103, distinct from WSA last-error 997),
real discovery requests (21 bytes) and session replies (731 bytes) were exchanged.
The native browser displayed the other player's named lobby. Joining and a
complete race still require validation. A meeting point on one client is not
proof that another client can join.
The direct System Link path must be tested before assuming it needs XGI search.

When -PadFile is supplied, physical input remains enabled. Automated tests may
explicitly add -ScriptedOnly to isolate scripted input. Do not use that switch
for manual controller testing; without -PadFile it is rejected.

A real two-process runtime transport self-test is also available:

    powershell -ExecutionPolicy Bypass -File scripts/test-lan-transport.ps1

This invokes --fm2_lan_selftest and exits before starting game code. It uses
kernel XSocket handles, the port's bind/send/receive and socket-query hooks,
registered session keys and a test hello/ack exchange. It uses UDP 39741, not
the gameplay port. Both processes passed this test with distinct offline XUIDs.
The test messages are our diagnostic protocol, not FM2 lobby/session packets.
Each lab launch writes a fresh session-<guid>.log to avoid stale test results.

Overlapped results use Xbox pending/HRESULT status and preserve stack-backed
immediate-result pointers after a pending return. Delayed results are retrieved
through WSAGetOverlappedResult; flags occupy offset 8 in WSAOVERLAPPED.

Broadcast is treated as a usable connectionless route by XNetConnect and
XNetGetConnectStatus so the game can submit its discovery sends. No guest
completion-event reset is performed when queuing a receive; the event may also
carry notifications for other queued network work.

## Join fix and diagnostics (2026-10-20)

FM2 networks through a DirectPlay8-style layer over XNet (HRESULTs 0x8015xxxx).
Joining used to fail locally with "session full or unavailable" before any
packet reached the host. The Connect command completed with E_OUTOFMEMORY
(sub_826E79F0) because sub_826E5690, which builds the host's DirectPlay URL,
calls XNetInAddrToXnAddr(ip, &xnaddr, NULL) and our hook rejected the null
key-ID output. The key ID is optional, as on the console; that fixed joining.

XNetRandom is also hooked in LAN mode: the runtime fills it with constant 0xBB
bytes, which would give both peers identical nonces.

Diagnostics kept in the LAN build (all gated by --fm2_lan):
* guest return addresses (lr) on hooked XNet calls and a guest back-chain
  stack on NetDll_shutdown;
* failed DirectPlay command HRESULTs (wrapper on sub_826D1288);
* arguments/results of sub_826DD9D8, sub_826E9C70 and sub_826E5690.
The unit tests define FM2_LAN_NO_GUEST_WRAPPERS because they link lan.cpp
without the recompiled title.

## Online latency: DirectPlay throttling (2026-10-20)

Over the VPS relay (~25 ms round trip) remote cars stuttered. Measured per
peer: direct LAN ~120 packets/s at a steady 8.3 ms; relay ~6/s with 0.5 s
stalls, while frame rate stayed at 60 fps and the relay dropped nothing.
A local relay (same code, ~0 ms) was smooth, so latency was the trigger.

FM2 embeds the DirectPlay8 protocol layer. Captures showed a strict
send/ack ping-pong (POLL on every frame), i.e. a one-frame send window:
retry timeouts count as drops (sub_826C98E0), which halve the window
(sub_826C9200, minimum 1) and pause its growth (sub_826C8E70). Endpoint
creation (sub_826C93C8) already has a native "throttling disabled" branch
(63-frame window, EPD+20 flag 0x400 that makes the drop counter a no-op).
In relay mode the port applies that branch to every endpoint. Result: ~60
data frames/s per peer (the game's own rate) with occasional ~100 ms gaps.

Diagnostics: `--frame_stats=true` logs fps; "FM2 LAN rx" logs per-peer
packet pacing every 5 s; fm2_relay.py `--trace FILE` logs datagram headers.

Open issue: with 3+ players a joiner occasionally misses one peer (sees
3 of 4 cars) and may crash at race start (null read 0x50 in sub_821E1598).
Rejoining works. Root cause not found yet.

## Online packet loss (2026-10-20)

After the throttling fix, remaining ~100-200 ms stalls came from lost frames:
DirectPlay does not resend unreliable frames and holds later ones until a
timer gives up. Captures on the VPS showed ~3 % of datagrams lost on the
player -> VPS uplink (none server-side or on the downlink); a synthetic
probe at 20-120 packets/s reproduced 1.3-3.4 % loss, a local relay 0 %.

The relay now carries optional loss recovery (types 0x12/0x13, advertised in
WELCOME): each unicast datagram includes a per-destination sequence number
and copies of the previous two. Same probe through the VPS: 0 of 1800 lost
per direction. Unit test: lan_tests covers drop-one-of-five recovery and
duplicate suppression. Not yet confirmed in an actual race.
