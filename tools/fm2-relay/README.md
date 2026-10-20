# FM2 relay

Online play for the FM2 port. The relay is a virtual System Link segment:
each client joins a room, receives a 10.68.x.y address and tunnels its game
UDP through the server. Broadcasts reach everyone in the room, so the game's
own System Link browser lists every session there. Players need no open ports.

## Client

Forza2Launcher has an Online card (Profiles tab), on by default, that writes
`fm2_relay = "<server>"` to forza2.user.toml (empty = offline). The server
is a build setting: set `FM2_ONLINE_SERVER=host:port` before `build-vulkan.bat`;
builds without it hide the card.
The card never shows the address: at startup it sends one JOIN (answered by a
CHALLENGE, no session created) and shows a green "Server available" dot.
By hand:

    forza2.exe --fm2_relay=host:port

Every client joins the single room `public`, so everyone sees every session
on the server. (The protocol still carries a room name; the client fixes it.)
If the relay does not answer at startup the game stays offline. For a local
two-window lab: `scripts/lan-lab.ps1 -Role host|guest -Relay host:port`.

## Server

Python 3 standard library only. `test_relay.py` starts a private instance and
checks join, bind, broadcast, unicast and room isolation.

Install on Ubuntu (as deployed on the VPS):

    sudo useradd --system --no-create-home --shell /usr/sbin/nologin fm2relay
    sudo install -d /opt/fm2-relay
    sudo install -m 644 fm2_relay.py /opt/fm2-relay/
    sudo install -m 644 fm2-relay.service /etc/systemd/system/
    sudo systemctl daemon-reload && sudo systemctl enable --now fm2-relay
    journalctl -u fm2-relay -f        # joins, leaves, stats every 5 min

UDP 23020 must be reachable. Options: `--max-room` (256), `--max-clients`
(256), `--rate`/`--burst` (packets/s per game socket).

## Protocol

All packets start with `F2 <type>`. Multi-byte fields are big-endian.

| type | direction | layout |
|------|-----------|--------|
| 01 JOIN | c→s | nonce[8] room_len room, zero-padded to 64 bytes |
| 02 CHALLENGE | s→c | nonce[8] cookie[16] (HMAC of endpoint+nonce, 60 s) |
| 03 JOIN2 | c→s | nonce[8] cookie[16] room_len room, padded to 64 |
| 04 WELCOME | s→c | nonce[8] token[8] address[4] caps[1] (bit 0: loss recovery) |
| 08 ERROR | s→c | nonce[8] 0 text (JOIN replies), or 8 zero bytes (unknown PING) |
| 05 BIND | c→s | token[8] port[2], from each game socket, every 10 s |
| 06 PING | c→s | token[8], session keepalive every 10 s |
| 10 DATA | c→s | dst_ip[4] dst_port[2] payload |
| 11 DATA | s→c | src_ip[4] src_port[2] payload |
| 12 DATA_R | c→s | dst_ip[4] dst_port[2] recovery payload |
| 13 DATA_R | s→c | src_ip[4] src_port[2] recovery payload |

Recovery payload (opaque to the server): `seq[2] count[1]` then `count`
entries of `len[2] bytes`, newest first: the current datagram and up to two
previous ones to the same destination, within 1200 bytes. Receivers deliver
entries newer than the last delivered seq, oldest first, and drop the rest.
Players' uplinks measured ~3 % UDP loss; with this framing 0 of 3600
datagrams were lost. Clients use it only when WELCOME advertises it, and
never for broadcasts.

The cookie round trip proves the client owns its address, and replies are
never larger than requests, so the server cannot be used to amplify spoofed
traffic. Data is accepted only from endpoints bound with a valid token.
`dst_ip` 255.255.255.255 fans out to the room. Sessions and bindings expire
after 45 s without traffic.
