#!/usr/bin/env python3
"""FM2 relay: a virtual System Link segment for the Forza Motorsport 2 port.

Each client joins a room and is given a 10.68.x.y address. Game datagrams are
tunnelled through this server; broadcasts reach every client in the room, so
the game's own System Link browser lists every session in it. Standard library
only. Protocol: see README.md next to this file.
"""
import argparse
import asyncio
import hashlib
import hmac
import ipaddress
import logging
import os
import re
import struct
import time

MAGIC = 0xF2
JOIN, CHALLENGE, JOIN2, WELCOME, BIND, PING, ERROR = 1, 2, 3, 4, 5, 6, 8
DATA_OUT, DATA_IN = 0x10, 0x11
# Same routing, but the payload carries the client's loss-recovery framing
# (sequence number plus copies of recent datagrams); the server never parses it.
DATA_OUT_R, DATA_IN_R = 0x12, 0x13
CAPS_REDUNDANT = 0x01  # WELCOME capability byte
HEADER = 8
JOIN_SIZE = 64
BROADCASTS = {0xFFFFFFFF, 0x0A44FFFF}
ROOM_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
SESSION_TIMEOUT = 45.0
COOKIE_WINDOW = 60

log = logging.getLogger("fm2-relay")


class Session:
    __slots__ = ("token", "room", "vip", "control", "seen", "nonce")

    def __init__(self, token, room, vip, control, nonce):
        self.token, self.room, self.vip = token, room, vip
        self.control, self.nonce = control, nonce
        self.seen = time.monotonic()


class Binding:
    __slots__ = ("session", "port", "endpoint", "seen", "tokens", "refill")

    def __init__(self, session, port, endpoint, rate):
        self.session, self.port, self.endpoint = session, port, endpoint
        self.seen = time.monotonic()
        self.tokens, self.refill = float(rate), time.monotonic()


class Relay(asyncio.DatagramProtocol):
    def __init__(self, args):
        self.args = args
        self.secret = os.urandom(32)
        self.sessions = {}  # token -> Session
        self.joins = {}  # (control endpoint, nonce) -> token, makes JOIN2 idempotent
        self.rooms = {}  # room -> set(Session)
        self.bindings = {}  # endpoint -> Binding
        self.routes = {}  # (room, vip, port) -> Binding
        self.free = [
            0x0A440000 | n for n in range(1, 0xFFFF) if n & 0xFF not in (0, 0xFF)
        ]
        self.free.reverse()
        self.stats = {"in": 0, "out": 0, "dropped": 0}
        # Optional diagnostics: one line per game datagram (time, source,
        # destination, size, first payload bytes), bounded by --trace-lines.
        self.trace = open(args.trace, "w", buffering=1) if args.trace else None
        self.trace_left = args.trace_lines

    def connection_made(self, transport):
        self.transport = transport

    def send(self, data, endpoint):
        self.transport.sendto(data, endpoint)
        self.stats["out"] += 1

    def error(self, endpoint, nonce, text):
        # Only sent in reply to 64-byte JOINs: never larger than the request.
        self.send(bytes([MAGIC, ERROR]) + nonce + b"\x00" + text.encode()[:50],
                  endpoint)

    def cookie(self, endpoint, nonce, bucket):
        message = f"{endpoint[0]}|{endpoint[1]}|{bucket}|".encode() + nonce
        return hmac.new(self.secret, message, hashlib.sha256).digest()[:16]

    # -- control ------------------------------------------------------------
    def handle_join(self, data, endpoint, with_cookie):
        if len(data) < JOIN_SIZE:
            return  # Padding keeps replies smaller than requests.
        nonce = data[2:10]
        offset = 10
        if with_cookie:
            cookie = data[10:26]
            offset = 26
            bucket = int(time.time()) // COOKIE_WINDOW
            if not any(hmac.compare_digest(cookie, self.cookie(endpoint, nonce, b))
                       for b in (bucket, bucket - 1)):
                return
        length = data[offset]
        room = data[offset + 1:offset + 1 + length].decode("ascii", "replace")
        if not ROOM_RE.match(room):
            self.error(endpoint, nonce, "invalid room name")
            return
        if not with_cookie:
            bucket = int(time.time()) // COOKIE_WINDOW
            self.send(bytes([MAGIC, CHALLENGE]) + nonce +
                      self.cookie(endpoint, nonce, bucket), endpoint)
            return
        token = self.joins.get((endpoint, nonce))
        session = self.sessions.get(token) if token else None
        if session is None:
            members = self.rooms.setdefault(room, set())
            if len(members) >= self.args.max_room:
                self.error(endpoint, nonce, f"room '{room}' is full")
                return
            if len(self.sessions) >= self.args.max_clients or not self.free:
                self.error(endpoint, nonce, "server is full")
                return
            session = Session(os.urandom(8), room, self.free.pop(), endpoint, nonce)
            self.sessions[session.token] = session
            self.joins[(endpoint, nonce)] = session.token
            members.add(session)
            log.info("join %s room=%s from %s:%d (%d in room)",
                     ipaddress.IPv4Address(session.vip), room, endpoint[0],
                     endpoint[1], len(members))
        session.seen = time.monotonic()
        self.send(bytes([MAGIC, WELCOME]) + nonce + session.token +
                  struct.pack(">I", session.vip) + bytes([CAPS_REDUNDANT]),
                  endpoint)

    def handle_bind(self, data, endpoint):
        if len(data) < 12:
            return
        session = self.sessions.get(data[2:10])
        if session is None:
            return
        port = struct.unpack(">H", data[10:12])[0]
        session.seen = time.monotonic()
        binding = self.bindings.get(endpoint)
        if binding and binding.session is session and binding.port == port:
            binding.seen = session.seen
            return
        if binding:
            self.drop_binding(binding)
        route = (session.room, session.vip, port)
        old = self.routes.get(route)
        if old:
            self.drop_binding(old)
        binding = Binding(session, port, endpoint, self.args.burst)
        self.bindings[endpoint] = binding
        self.routes[route] = binding

    def handle_ping(self, data, endpoint):
        if len(data) < 10:
            return
        session = self.sessions.get(data[2:10])
        if session is None:
            # Same size as the PING, so it cannot amplify spoofed traffic.
            self.send(bytes([MAGIC, ERROR]) + b"\x00" * 8, endpoint)
        else:
            session.seen = time.monotonic()

    # -- data ---------------------------------------------------------------
    def allow(self, binding, now):
        binding.tokens = min(self.args.burst, binding.tokens +
                             (now - binding.refill) * self.args.rate)
        binding.refill = now
        if binding.tokens < 1:
            return False
        binding.tokens -= 1
        return True

    def handle_data(self, data, endpoint, kind=DATA_OUT):
        binding = self.bindings.get(endpoint)
        if binding is None:
            self.stats["dropped"] += 1
            return
        now = time.monotonic()
        if not self.allow(binding, now):
            self.stats["dropped"] += 1
            return
        session = binding.session
        binding.seen = session.seen = now
        dst_ip, dst_port = struct.unpack(">IH", data[2:8])
        if self.trace and self.trace_left > 0:
            self.trace_left -= 1
            self.trace.write(f"{now:.4f} {session.vip & 0xFFFF:x} {dst_ip & 0xFFFF:x} "
                             f"{len(data) - HEADER} {data[HEADER:HEADER + 8].hex()}\n")
            if self.trace_left == 0:
                self.trace.close()
                self.trace = None
        out_kind = DATA_IN if kind == DATA_OUT else DATA_IN_R
        packet = (bytes([MAGIC, out_kind]) +
                  struct.pack(">IH", session.vip, binding.port) + data[HEADER:])
        if dst_ip in BROADCASTS:
            for member in self.rooms.get(session.room, ()):
                if member is session:
                    continue
                target = self.routes.get((session.room, member.vip, dst_port))
                if target:
                    self.send(packet, target.endpoint)
            return
        target = self.routes.get((session.room, dst_ip, dst_port))
        if target:
            self.send(packet, target.endpoint)
        else:
            self.stats["dropped"] += 1

    def datagram_received(self, data, endpoint):
        self.stats["in"] += 1
        if len(data) < 2 or data[0] != MAGIC or len(data) > 2048:
            return
        kind = data[1]
        if kind in (DATA_OUT, DATA_OUT_R) and len(data) >= HEADER:
            self.handle_data(data, endpoint, kind)
        elif kind == BIND:
            self.handle_bind(data, endpoint)
        elif kind == PING:
            self.handle_ping(data, endpoint)
        elif kind in (JOIN, JOIN2):
            self.handle_join(data, endpoint, kind == JOIN2)

    # -- housekeeping -------------------------------------------------------
    def drop_binding(self, binding):
        self.bindings.pop(binding.endpoint, None)
        route = (binding.session.room, binding.session.vip, binding.port)
        if self.routes.get(route) is binding:
            del self.routes[route]

    def expire(self):
        now = time.monotonic()
        for binding in [b for b in self.bindings.values()
                        if now - b.seen > SESSION_TIMEOUT]:
            self.drop_binding(binding)
        for session in [s for s in self.sessions.values()
                        if now - s.seen > SESSION_TIMEOUT]:
            for binding in [b for b in self.bindings.values()
                            if b.session is session]:
                self.drop_binding(binding)
            del self.sessions[session.token]
            self.joins.pop((session.control, session.nonce), None)
            members = self.rooms.get(session.room)
            if members is not None:
                members.discard(session)
                if not members:
                    del self.rooms[session.room]
            self.free.append(session.vip)
            log.info("leave %s room=%s", ipaddress.IPv4Address(session.vip),
                     session.room)


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=23020)
    parser.add_argument("--max-room", type=int, default=256,
                        help="players per room; the client always joins 'public'")
    parser.add_argument("--max-clients", type=int, default=256)
    # In a race FM2 sends ~120 packets/s to each opponent; 8 players need ~850.
    parser.add_argument("--rate", type=float, default=2000,
                        help="sustained packets/s allowed per game socket")
    parser.add_argument("--burst", type=float, default=4000)
    parser.add_argument("--trace", help="write a per-datagram header trace here")
    parser.add_argument("--trace-lines", type=int, default=300000)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    loop = asyncio.get_running_loop()
    transport, relay = await loop.create_datagram_endpoint(
        lambda: Relay(args), local_addr=(args.bind, args.port))
    log.info("listening on udp %s:%d", args.bind, args.port)
    last_stats = time.monotonic()
    try:
        while True:
            await asyncio.sleep(5)
            relay.expire()
            if time.monotonic() - last_stats >= 300:
                last_stats = time.monotonic()
                log.info("stats players=%d rooms=%d packets in=%d out=%d dropped=%d",
                         len(relay.sessions), len(relay.rooms), relay.stats["in"],
                         relay.stats["out"], relay.stats["dropped"])
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
