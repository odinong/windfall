"""Windfall multiplayer server: a headless UDP relay with room codes (Python 3.9+, no dependencies).

  python3 windfall_server.py [--host 0.0.0.0] [--port 27015] [--ws-host 127.0.0.1] [--ws-port 27016]

Players connect by UDP, or by WebSocket (wss://<hostname>, each binary message one packet) where only HTTPS
reaches the server: behind a NAT without a UDP port forward, through a Cloudflare tunnel or a reverse proxy
pointed at the WebSocket port (local only by default, as the tunnel or proxy connects to it on this machine).

Each room holds up to 8 players. Players stream the state of the network views they own (Photon-style:
each view has an owner and an id; the owner sends, everyone else replicates); the server relays every
view state to the rest of the room and keeps the room's player list. Packets (all little-endian):

  client -> server
    J  u8 proto, char room[16], char name[16]                join (resent until W arrives)
    S  u8 id, u32 seq, u8 count, views...                    owned views {u16 id, u8 kind, u16 len, data},
                                                             relayed as is (kind 1: Link, 2: pose, 3: rigid models)
    C  u8 id, utf-8 text                                     chat, relayed with the sender's name
    E  u8 id, u32 seq, u8 kind, payload                      game event (inventory, flags), relayed
    K  u8 id                                                 keep-alive
    L  u8 id                                                 leave
  server -> client
    W  u8 id, u8 color                                       welcome (your player id and tunic color)
    P  u8 count, {u8 id, u8 color, char name[16]}...         the room's players
    S / E                                                    relayed from the other players
    C  u8 id, char name[16], utf-8 text                      chat
    F  utf-8 reason                                          join refused (room full, bad protocol)
"""
import argparse
import asyncio
import base64
import hashlib
import logging
import struct
import time

PROTO = 9
MAX_PLAYERS = 8
TIMEOUT = 10.0
COLORS = list(range(MAX_PLAYERS))
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_BACKLOG = 1 << 20

def cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")

def pad(s: str, n: int) -> bytes:
    return s.encode("utf-8")[: n - 1].ljust(n, b"\0")

class Player:
    def __init__(self, pid, addr, name, color):
        self.id, self.addr, self.name, self.color = pid, addr, name, color
        self.last_seen = time.monotonic()

class Room:
    def __init__(self, code):
        self.code = code
        self.players = {}

    def free_id(self):
        for i in range(MAX_PLAYERS):
            if i not in self.players:
                return i
        return None

    def roster(self) -> bytes:
        out = b"P" + bytes([len(self.players)])
        for p in sorted(self.players.values(), key=lambda p: p.id):
            out += bytes([p.id, p.color]) + pad(p.name, 16)
        return out

class Server(asyncio.DatagramProtocol):
    def __init__(self):
        self.rooms = {}
        self.by_addr = {}
        self.ws = {}
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def error_received(self, exc):
        logging.debug("udp error: %s", exc)

    def send(self, addr, data: bytes):
        writer = self.ws.get(addr)
        if writer is None:
            self.transport.sendto(data, addr)
        elif writer.transport.get_write_buffer_size() < WS_BACKLOG:
            writer.write(ws_frame(data))

    def broadcast(self, room, data: bytes, skip=None):
        for p in room.players.values():
            if p is not skip:
                self.send(p.addr, data)

    def leave(self, addr, why):
        entry = self.by_addr.pop(addr, None)
        if not entry:
            return
        room, player = entry
        room.players.pop(player.id, None)
        logging.info("room %s: %s (%d) left (%s)", room.code, player.name, player.id, why)
        if room.players:
            self.broadcast(room, room.roster())
        else:
            self.rooms.pop(room.code, None)

    def datagram_received(self, data: bytes, addr):
        if not data:
            return
        kind = data[:1]
        if kind == b"J":
            self.join(data, addr)
            return
        entry = self.by_addr.get(addr)
        if not entry:
            return
        room, player = entry
        player.last_seen = time.monotonic()
        if len(data) < 2 or data[1] != player.id:
            return
        if kind in (b"S", b"E"):
            self.broadcast(room, data, skip=player)
        elif kind == b"C":
            text = data[2:258].decode("utf-8", "replace")
            logging.info("room %s chat %s: %s", room.code, player.name, text)
            self.broadcast(room, b"C" + bytes([player.id]) + pad(player.name, 16) + text.encode("utf-8"))
        elif kind == b"L":
            self.leave(addr, "left")

    def join(self, data: bytes, addr):
        if len(data) < 34 or data[1] != PROTO:
            self.send(addr, b"F" + b"protocol mismatch: update Windfall")
            return
        code = cstr(data[2:18]).upper() or "PUBLIC"
        name = cstr(data[18:34]) or "Link"
        if addr in self.by_addr:
            room, player = self.by_addr[addr]
            if room.code == code:
                self.send(addr, b"W" + bytes([player.id, player.color]))
                self.send(addr, room.roster())
                return
            self.leave(addr, "changed room")
        room = self.rooms.setdefault(code, Room(code))
        pid = room.free_id()
        if pid is None:
            self.send(addr, b"F" + b"room is full")
            return
        player = Player(pid, addr, name, COLORS[pid])
        room.players[pid] = player
        self.by_addr[addr] = (room, player)
        logging.info("room %s: %s joined as player %d from %s", code, name, pid, addr)
        self.send(addr, b"W" + bytes([pid, player.color]))
        self.broadcast(room, room.roster())

    async def reap(self):
        while True:
            await asyncio.sleep(1.0)
            now = time.monotonic()
            for addr, (room, player) in list(self.by_addr.items()):
                if now - player.last_seen > TIMEOUT:
                    self.leave(addr, "timed out")

def ws_frame(data: bytes, opcode=2) -> bytes:
    n = len(data)
    if n < 126:
        head = bytes([0x80 | opcode, n])
    elif n < 65536:
        head = bytes([0x80 | opcode, 126]) + struct.pack(">H", n)
    else:
        head = bytes([0x80 | opcode, 127]) + struct.pack(">Q", n)
    return head + data

def unmask(data: bytes, mask: bytes) -> bytes:
    key = int.from_bytes((mask * (len(data) // 4 + 1))[: len(data)], "little")
    return (int.from_bytes(data, "little") ^ key).to_bytes(len(data), "little")

async def ws_session(server, reader, writer, counter=[0]):
    addr = None
    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        headers = {}
        for line in head.decode("latin-1").split("\r\n")[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        key = headers.get("sec-websocket-key")
        if not key or "websocket" not in headers.get("upgrade", "").lower():
            writer.write(b"HTTP/1.1 426 Upgrade Required\r\nContent-Length: 18\r\n\r\nWindfall server ok")
            return
        accept = base64.b64encode(hashlib.sha1(key.encode() + WS_GUID).digest()).decode()
        writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     b"Sec-WebSocket-Accept: " + accept.encode() + b"\r\n\r\n")
        ip = (headers.get("cf-connecting-ip") or headers.get("x-forwarded-for", "").split(",")[0].strip()
              or writer.get_extra_info("peername", ("?",))[0])
        counter[0] += 1
        addr = ("ws", ip, counter[0])
        server.ws[addr] = writer
        msg = b""
        while True:
            b0, b1 = await reader.readexactly(2)
            op, n = b0 & 15, b1 & 127
            if n == 126:
                n = struct.unpack(">H", await reader.readexactly(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", await reader.readexactly(8))[0]
            if n > WS_BACKLOG:
                return
            mask = await reader.readexactly(4) if b1 & 128 else b""
            data = await reader.readexactly(n)
            if mask:
                data = unmask(data, mask)
            if op == 8:
                return
            if op == 9:
                writer.write(ws_frame(data, 10))
                continue
            if op == 10:
                continue
            msg = msg + data if op == 0 else data
            if b0 & 0x80:
                server.datagram_received(msg, addr)
                msg = b""
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError, ConnectionError):
        pass
    finally:
        if addr:
            server.ws.pop(addr, None)
            server.leave(addr, "disconnected")
        writer.close()

async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=27015)
    ap.add_argument("--ws-host", default="127.0.0.1")
    ap.add_argument("--ws-port", type=int, default=27016, help="WebSocket port (0: none)")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    loop = asyncio.get_running_loop()
    transport, server = await loop.create_datagram_endpoint(Server, local_addr=(args.host, args.port))
    logging.info("Windfall server on %s:%d (udp)", args.host, args.port)
    if args.ws_port:
        await asyncio.start_server(lambda r, w: ws_session(server, r, w), args.ws_host, args.ws_port)
        logging.info("Windfall server on %s:%d (websocket)", args.ws_host, args.ws_port)
    try:
        await server.reap()
    finally:
        transport.close()

if __name__ == "__main__":
    import sys
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
