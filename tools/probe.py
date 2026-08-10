#!/usr/bin/env python3
"""Minimal RTSP-over-TCP health probe for an OneKVM video stream."""

import argparse
import re
import socket
import time


class Client:
    def __init__(self, host: str, port: int, timeout: float) -> None:
        self.socket = socket.create_connection((host, port), timeout)
        self.socket.settimeout(timeout)
        self.buffer = b""
        self.cseq = 0

    def _discard_interleaved(self) -> bool:
        while self.buffer.startswith(b"$"):
            if len(self.buffer) < 4:
                return False
            size = int.from_bytes(self.buffer[2:4], "big")
            if len(self.buffer) < 4 + size:
                return False
            self.buffer = self.buffer[4 + size :]
        return True

    def request(self, method: str, url: str, **headers: str) -> tuple[str, bytes]:
        self.cseq += 1
        lines = [
            f"{method} {url} RTSP/1.0",
            f"CSeq: {self.cseq}",
            "User-Agent: OneKVM-RTSP-probe/1.0",
            *(f"{key}: {value}" for key, value in headers.items()),
        ]
        self.socket.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        while True:
            self._discard_interleaved()
            if self.buffer.startswith(b"RTSP/") and b"\r\n\r\n" in self.buffer:
                break
            self.buffer += self.socket.recv(65536)
        raw_header, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        match = re.search(br"Content-Length:\s*(\d+)", raw_header, re.I)
        content_length = int(match.group(1)) if match else 0
        while len(self.buffer) < content_length:
            self.buffer += self.socket.recv(65536)
        body, self.buffer = (
            self.buffer[:content_length],
            self.buffer[content_length:],
        )
        header = raw_header.decode("ascii", "replace")
        status = header.split("\r\n", 1)[0]
        if " 200 " not in status:
            raise RuntimeError(f"{method}: {status}")
        return header, body


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=8554)
    parser.add_argument("--path", default="/onekvm")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--min-fps", type=float, default=1)
    args = parser.parse_args()

    client = Client(args.host, args.port, args.timeout)
    base_url = f"rtsp://{args.host}:{args.port}/{args.path.lstrip('/')}"
    client.request("OPTIONS", base_url)
    _, body = client.request("DESCRIBE", base_url, Accept="application/sdp")
    sdp = body.decode("ascii", "replace")
    controls = re.findall(r"^a=control:(.+)$", sdp, re.MULTILINE)
    control = next((value.strip() for value in controls if value.strip() != "*"), None)
    if not control:
        raise RuntimeError("DESCRIBE: SDP has no media control URL")
    track_url = (
        control
        if control.startswith("rtsp://")
        else f"{base_url.rstrip('/')}/{control.lstrip('/')}"
    )
    setup_header, _ = client.request(
        "SETUP",
        track_url,
        Transport="RTP/AVP/TCP;unicast;interleaved=0-1",
    )
    match = re.search(r"^Session:\s*([^;\r\n]+)", setup_header, re.I | re.M)
    if not match:
        raise RuntimeError("SETUP: response has no session ID")
    session = match.group(1).strip()
    client.request("PLAY", base_url, Session=session)
    print(f"play=ok track={control}", flush=True)

    client.socket.settimeout(1)
    started = time.monotonic()
    deadline = started + args.seconds
    packets = frames = payload_bytes = 0
    while time.monotonic() < deadline:
        try:
            chunk = client.socket.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        client.buffer += chunk
        while client.buffer:
            if client.buffer[0] != ord("$"):
                marker = client.buffer.find(b"$")
                if marker < 0:
                    client.buffer = b""
                    break
                client.buffer = client.buffer[marker:]
            if len(client.buffer) < 4:
                break
            size = int.from_bytes(client.buffer[2:4], "big")
            if len(client.buffer) < 4 + size:
                break
            channel = client.buffer[1]
            packet = client.buffer[4 : 4 + size]
            client.buffer = client.buffer[4 + size :]
            if channel == 0 and len(packet) >= 12:
                packets += 1
                frames += packet[1] >> 7
                payload_bytes += len(packet) - 12

    elapsed = time.monotonic() - started
    fps = frames / elapsed
    bitrate_kbps = payload_bytes * 8 / elapsed / 1000
    print(
        f"rtp_packets={packets} frames={frames} fps={fps:.2f} "
        f"payload_bytes={payload_bytes} kbps={bitrate_kbps:.1f}"
    )
    client.socket.close()
    if fps < args.min_fps:
        raise SystemExit(f"FPS {fps:.2f} is below required {args.min_fps:.2f}")


if __name__ == "__main__":
    main()
