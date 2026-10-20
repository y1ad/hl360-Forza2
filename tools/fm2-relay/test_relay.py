#!/usr/bin/env python3
"""Protocol test: starts fm2_relay.py on a free port and drives three clients."""
import os
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = 0xF2


def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(2)
    return s


def join(server, room):
    control = udp()
    nonce = os.urandom(8)
    room_bytes = bytes([len(room)]) + room.encode()
    control.sendto((bytes([MAGIC, 1]) + nonce + room_bytes).ljust(64, b"\0"), server)
    reply, _ = control.recvfrom(256)
    assert reply[1] == 2 and reply[2:10] == nonce and len(reply) <= 64, reply
    cookie = reply[10:26]
    control.sendto((bytes([MAGIC, 3]) + nonce + cookie + room_bytes).ljust(64, b"\0"),
                   server)
    reply, _ = control.recvfrom(256)
    assert reply[1] == 4 and reply[2:10] == nonce, reply
    token, vip = reply[10:18], struct.unpack(">I", reply[18:22])[0]
    assert vip >> 16 == 0x0A44
    return control, token, vip


def bind(server, token, port):
    s = udp()
    s.sendto(bytes([MAGIC, 5]) + token + struct.pack(">H", port), server)
    return s


def data(dst, port, payload):
    return bytes([MAGIC, 0x10]) + struct.pack(">IH", dst, port) + payload


def expect(sock, src, port, payload):
    reply, _ = sock.recvfrom(4096)
    assert reply[:2] == bytes([MAGIC, 0x11]), reply
    assert struct.unpack(">IH", reply[2:8]) == (src, port), reply[2:8]
    assert reply[8:] == payload, reply


def silent(sock):
    sock.settimeout(0.3)
    try:
        sock.recvfrom(4096)
    except socket.timeout:
        return True
    finally:
        sock.settimeout(2)
    return False


def main():
    probe = udp()
    port = probe.getsockname()[1]
    probe.close()
    server = ("127.0.0.1", port)
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "fm2_relay.py"),
                              "--bind", "127.0.0.1", "--port", str(port)])
    try:
        time.sleep(0.8)
        _, token_a, vip_a = join(server, "test")
        _, token_b, vip_b = join(server, "test")
        _, token_c, vip_c = join(server, "other")
        assert len({vip_a, vip_b, vip_c}) == 3
        game_a = bind(server, token_a, 2302)
        game_b = bind(server, token_b, 2302)
        game_c = bind(server, token_c, 2302)
        time.sleep(0.2)
        # Broadcast discovery reaches the room, not the sender or other rooms.
        game_b.sendto(data(0xFFFFFFFF, 2302, b"discover"), server)
        expect(game_a, vip_b, 2302, b"discover")
        assert silent(game_b) and silent(game_c)
        # Unicast both ways.
        game_a.sendto(data(vip_b, 2302, b"x" * 731), server)
        expect(game_b, vip_a, 2302, b"x" * 731)
        game_b.sendto(data(vip_a, 2302, b"join"), server)
        expect(game_a, vip_b, 2302, b"join")
        # No cross-room unicast and no traffic from unbound endpoints.
        game_c.sendto(data(vip_a, 2302, b"leak"), server)
        stranger = udp()
        stranger.sendto(data(vip_a, 2302, b"spoof"), server)
        assert silent(game_a)
        # A forged BIND without the session token is ignored.
        forged = bind(server, b"\0" * 8, 2302)
        forged.sendto(data(vip_a, 2302, b"forged"), server)
        assert silent(game_a)
        print("PASS: join/challenge, bind, room broadcast, unicast, isolation")
    finally:
        relay.terminate()
        relay.wait()


if __name__ == "__main__":
    main()
