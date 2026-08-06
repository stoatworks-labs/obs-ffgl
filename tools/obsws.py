"""A obs-websocket 5 client in the standard library and nothing else.

Verifying this plugin means driving a real OBS: there is no way to render an
FFGL plugin through libobs's graphics pipeline without libobs, and no way to
get a frame back out of OBS except by asking it. obs-websocket ships with OBS
and is already enabled here, so it is the shortest path to a picture.

Depending on `websocket-client` would put a pip install between a clean
checkout and its own test suite, so the ~90 lines of framing are written out
instead. Only what this needs is implemented: a client-side handshake, masked
text frames out, unmasked frames in, and no fragmentation (obs-websocket does
not fragment its responses).
"""

import base64
import hashlib
import json
import secrets
import socket
import struct


class WebSocket:
    def __init__(self, host: str, port: int, timeout: float = 15.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        self.sock.sendall(
            (
                f"GET / HTTP/1.1\r\n"
                f"Host: {host}:{port}\r\n"
                f"Upgrade: websocket\r\n"
                f"Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                f"Sec-WebSocket-Version: 13\r\n"
                f"\r\n"
            ).encode()
        )

        header = b""
        while b"\r\n\r\n" not in header:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed during handshake")
            header += chunk
        if b"101" not in header.split(b"\r\n", 1)[0]:
            raise ConnectionError(f"handshake refused: {header.split(chr(13).encode())[0]!r}")

        # Anything after the header belongs to the first frame.
        self._buffer = header.split(b"\r\n\r\n", 1)[1]

    # -- framing ----------------------------------------------------------
    def _read(self, count: int) -> bytes:
        while len(self._buffer) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed")
            self._buffer += chunk
        out, self._buffer = self._buffer[:count], self._buffer[count:]
        return out

    def send(self, payload: dict) -> None:
        data = json.dumps(payload).encode()
        # FIN + opcode 1 (text). Client frames MUST be masked (RFC 6455 §5.1);
        # a server that follows the spec drops an unmasked one and closes.
        frame = bytearray([0x81])
        length = len(data)
        if length < 126:
            frame.append(0x80 | length)
        elif length < 65536:
            frame.append(0x80 | 126)
            frame += struct.pack(">H", length)
        else:
            frame.append(0x80 | 127)
            frame += struct.pack(">Q", length)
        mask = secrets.token_bytes(4)
        frame += mask
        frame += bytes(byte ^ mask[i % 4] for i, byte in enumerate(data))
        self.sock.sendall(frame)

    def recv(self) -> dict:
        while True:
            first, second = self._read(2)
            opcode = first & 0x0F
            length = second & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            # Server-to-client frames are never masked.
            payload = self._read(length)

            if opcode == 0x8:
                raise ConnectionError("server sent close")
            if opcode == 0x9:  # ping -> pong
                self.sock.sendall(bytes([0x8A, 0x80]) + secrets.token_bytes(4))
                continue
            if opcode in (0x1, 0x2):
                return json.loads(payload)

    def close(self) -> None:
        try:
            self.sock.sendall(bytes([0x88, 0x80]) + secrets.token_bytes(4))
        except OSError:
            pass
        self.sock.close()


class Obs:
    """obs-websocket 5 request/response, with the SHA256 auth dance."""

    def __init__(self, password: str, host: str = "127.0.0.1", port: int = 4455):
        self.ws = WebSocket(host, port)
        hello = self.ws.recv()
        if hello.get("op") != 0:
            raise RuntimeError(f"expected Hello, got op {hello.get('op')}")

        identify = {"op": 1, "d": {"rpcVersion": 1}}
        auth = hello["d"].get("authentication")
        if auth:
            secret = base64.b64encode(
                hashlib.sha256((password + auth["salt"]).encode()).digest()
            ).decode()
            identify["d"]["authentication"] = base64.b64encode(
                hashlib.sha256((secret + auth["challenge"]).encode()).digest()
            ).decode()
        self.ws.send(identify)

        identified = self.ws.recv()
        if identified.get("op") != 2:
            raise RuntimeError(f"authentication refused: {identified}")
        self._counter = 0

    def call(self, request_type: str, data: dict | None = None) -> dict:
        self._counter += 1
        request_id = f"r{self._counter}"
        self.ws.send(
            {
                "op": 6,
                "d": {
                    "requestType": request_type,
                    "requestId": request_id,
                    "requestData": data or {},
                },
            }
        )
        while True:
            message = self.ws.recv()
            # Events (op 5) arrive unbidden and are not what anyone asked for.
            if message.get("op") != 7:
                continue
            body = message["d"]
            if body.get("requestId") != request_id:
                continue
            status = body["requestStatus"]
            if not status["result"]:
                raise RuntimeError(
                    f"{request_type} failed: {status.get('comment') or status['code']}"
                )
            return body.get("responseData") or {}

    def close(self) -> None:
        self.ws.close()


def password_from_config() -> str:
    """OBS keeps the websocket password in its own plugin config."""
    import pathlib

    path = (
        pathlib.Path.home()
        / "Library/Application Support/obs-studio/plugin_config/obs-websocket/config.json"
    )
    return json.loads(path.read_text())["server_password"]
