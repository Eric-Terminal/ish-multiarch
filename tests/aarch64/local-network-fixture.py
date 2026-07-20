#!/usr/bin/env python3

from __future__ import annotations

import argparse
import http.server
import os
import secrets
import signal
import socket
import struct
import threading
from pathlib import Path


KNOWN_NAMES = {
    "probe.ish-dns.test.",
    "getent.ish-dns.test.",
    "nslookup.ish-dns.test.",
    "http.ish-dns.test.",
}


def read_question(packet: bytes) -> tuple[str, int, int, int]:
    offset = 12
    labels: list[str] = []
    while True:
        if offset >= len(packet):
            raise ValueError("DNS 问题名称越过报文")
        length = packet[offset]
        offset += 1
        if length == 0:
            break
        if length & 0xC0 or offset + length > len(packet):
            raise ValueError("测试夹具不接受压缩或越界的问题名称")
        labels.append(packet[offset:offset + length].decode("ascii"))
        offset += length
    if offset + 4 > len(packet):
        raise ValueError("DNS 问题缺少类型或类别")
    query_type, query_class = struct.unpack_from("!HH", packet, offset)
    return ".".join(labels).lower() + ".", query_type, query_class, offset + 4


def response_for(packet: bytes) -> tuple[bytes, str, int]:
    if len(packet) < 12:
        raise ValueError("DNS 报文短于固定头部")
    query_id, flags, question_count, _, _, _ = struct.unpack_from(
        "!HHHHHH", packet)
    if question_count != 1:
        raise ValueError("测试夹具只接受单问题查询")

    name, query_type, query_class, question_end = read_question(packet)
    question = packet[12:question_end]
    response_flags = 0x8080 | (flags & 0x0100)
    answer = b""
    answer_count = 0
    if name not in KNOWN_NAMES or query_class != 1:
        response_flags |= 3
    elif query_type == 1:
        answer_count = 1
        answer = b"\xc0\x0c" + struct.pack(
            "!HHIH4s", 1, 1, 30, 4, socket.inet_aton("127.0.0.1"))
    header = struct.pack(
        "!HHHHHH", query_id, response_flags, 1, answer_count, 0, 0)
    return header + question + answer, name, query_type


def query_type_name(query_type: int) -> str:
    if query_type == 1:
        return "A"
    if query_type == 28:
        return "AAAA"
    return str(query_type)


def publish_ready(path: Path, dns_port: int,
        http_port: int, proof: str) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        f"DNS_PORT={dns_port}\nHTTP_PORT={http_port}\nPROOF={proof}\n",
        encoding="ascii")
    os.replace(temporary, path)


def make_http_server(proof: str) -> http.server.ThreadingHTTPServer:
    body = (proof + "\n").encode("ascii")

    class ProofHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path != "/proof":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=us-ascii")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *arguments: object) -> None:
            del format, arguments

    return http.server.ThreadingHTTPServer(("127.0.0.1", 0), ProofHandler)


def serve_dns(server: socket.socket, stop: threading.Event,
        query_log: Path) -> None:
    server.settimeout(0.2)
    with query_log.open("w", encoding="ascii", buffering=1) as log:
        while not stop.is_set():
            try:
                packet, peer = server.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                if stop.is_set():
                    break
                raise
            try:
                response, name, query_type = response_for(packet)
            except (UnicodeDecodeError, ValueError, struct.error):
                continue
            log.write(f"{name} {query_type_name(query_type)}\n")
            server.sendto(response, peer)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="提供完全本地的 AArch64 DNS 与 HTTP 验收服务")
    parser.add_argument("--ready-file", type=Path, required=True)
    parser.add_argument("--query-log", type=Path, required=True)
    parser.add_argument("--parent-pid", type=int, required=True)
    arguments = parser.parse_args()

    arguments.ready_file.unlink(missing_ok=True)
    proof = "ish-local-network-" + secrets.token_hex(12)
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda _signal, _frame: stop.set())
    signal.signal(signal.SIGINT, lambda _signal, _frame: stop.set())

    dns_server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dns_server.bind(("127.0.0.1", 0))
    http_server = make_http_server(proof)
    while http_server.server_address[1] == dns_server.getsockname()[1]:
        http_server.server_close()
        http_server = make_http_server(proof)
    dns_thread = threading.Thread(target=serve_dns,
            args=(dns_server, stop, arguments.query_log), daemon=True)
    http_thread = threading.Thread(
            target=http_server.serve_forever, daemon=True)
    dns_thread.start()
    http_thread.start()
    publish_ready(arguments.ready_file,
            dns_server.getsockname()[1],
            http_server.server_address[1], proof)

    try:
        while not stop.wait(0.2):
            if os.getppid() != arguments.parent_pid:
                break
    finally:
        stop.set()
        http_server.shutdown()
        dns_server.close()
        dns_thread.join(timeout=2)
        http_thread.join(timeout=2)
        http_server.server_close()


if __name__ == "__main__":
    main()
