#!/usr/bin/env python3
"""Run BlizzardUrlTest against loopback-only ephemeral HTTP fixtures."""
import http.server
import os
import socket
import subprocess
import sys
import threading
import time

DATA = b"0123456789abcdef"
STAMP = "Wed, 21 Oct 2015 07:28:00 GMT"

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def do_HEAD(self):
        self.reply(200, DATA, head=True)
    def do_POST(self):
        if self.path in ("/slowpost", "/snapshot"):
            time.sleep(0.2)
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        result = body + b"|" + self.headers.get("Content-Type", "").encode()
        if self.path == "/snapshot":
            result += b"|" + self.headers.get("If-Modified-Since", "").encode() + b"|" + self.headers.get("Cookie", "").encode()
        self.reply(200, result)
    def reply(self, code, body=b"", head=False, headers=()):
        self.send_response(code)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Last-Modified", STAMP)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)
            self.wfile.flush()
    def do_GET(self):
        try:
            path = self.path
            if path == "/stall":
                time.sleep(0.6)
                self.reply(200, DATA)
            elif path in ("/chunks", "/active"):
                self.send_response(200)
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for chunk in (DATA[:8], DATA[8:]):
                    self.wfile.write(b"8\r\n" + chunk + b"\r\n")
                    self.wfile.flush()
                    time.sleep(0.07 if path == "/active" else 0.001)
                if path == "/active":
                    for _ in range(4):
                        self.wfile.write(b"1\r\nx\r\n")
                        self.wfile.flush()
                        time.sleep(0.07)
                self.wfile.write(b"0\r\n\r\n")
            elif path.startswith("/status/"):
                self.reply(int(path.rsplit("/", 1)[1]))
            elif path == "/conditional":
                self.reply(304 if self.headers.get("If-Modified-Since") == STAMP else 200)
            elif path == "/redirect":
                self.reply(302, headers=[("Location", "/full")])
            elif path == "/unsafe":
                self.reply(302, headers=[("Location", "file:///not-accessed")])
            elif path.startswith("/set/"):
                name = path.rsplit("/", 1)[1]
                self.reply(200, headers=[("Set-Cookie", name + "=value; Path=/")])
            elif path in ("/delete", "/slowdelete"):
                if path == "/slowdelete":
                    time.sleep(0.2)
                self.reply(200, headers=[("Set-Cookie", "persist=; Max-Age=0; Path=/")])
            elif path == "/replace":
                self.reply(200, headers=[("Set-Cookie", "persist=newvalue; Path=/")])
            elif path == "/expired":
                self.reply(200, headers=[("Set-Cookie", "expired=yes; Expires=Wed, 21 Oct 2015 07:28:00 GMT; Path=/")])
            elif path == "/scoped":
                self.reply(200, headers=[("Set-Cookie", "scoped=yes; Path=/private"),
                                         ("Set-Cookie", "secret=yes; Secure; Path=/"),
                                         ("Set-Cookie", "alien=yes; Domain=invalid.example; Path=/")])
            elif path.endswith("/cookies"):
                if path == "/stall/cookies":
                    time.sleep(0.2)
                self.reply(200, self.headers.get("Cookie", "").encode())
            elif self.headers.get("Range"):
                start, end = self.headers["Range"].removeprefix("bytes=").split("-")
                part = DATA[int(start):int(end) + 1] if start and end else DATA[-int(end):] if end else DATA[int(start):]
                if start and not end:
                    part = DATA[int(start):]
                self.reply(206, part, headers=[("Content-Range", "bytes 2-5/16")])
            else:
                self.reply(200, DATA)
        except (BrokenPipeError, ConnectionResetError):
            pass

class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 64
    def handle_error(self, request, client_address):
        pass

class ConnectProxy(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def do_CONNECT(self):
        # Deliberately never resolve, connect or forward to the requested target.
        self.server.targets.append(self.path)
        self.send_response(200 if self.path == "127.0.0.1:1" else 400, "Connection established")
        self.send_header("Last-Modified", STAMP)
        self.send_header("Content-Length", "0")
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True  # Fail TLS immediately after the CONNECT response.

with Server(("127.0.0.1", 0), Handler) as server, Server(("127.0.0.2", 0), Handler) as other:
    # Reserved but not listening: deterministic refused loopback port.
    with socket.socket() as refused:
        refused.bind(("127.0.0.1", 0))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        threading.Thread(target=other.serve_forever, daemon=True).start()
        env = dict(os.environ, NO_PROXY="*", no_proxy="*")
        result = subprocess.run([sys.argv[1], str(server.server_port), str(refused.getsockname()[1]), str(other.server_port)], env=env, timeout=30)
        server.shutdown()
        other.shutdown()
        with Server(("127.0.0.1", 0), ConnectProxy) as proxy:
            proxy.targets = []
            threading.Thread(target=proxy.serve_forever, daemon=True).start()
            proxy_url = "http://127.0.0.1:" + str(proxy.server_port)
            proxy_env = dict(os.environ, NO_PROXY="", no_proxy="", HTTPS_PROXY=proxy_url, https_proxy=proxy_url)
            for key in ("ALL_PROXY", "all_proxy", "HTTP_PROXY", "http_proxy"):
                proxy_env.pop(key, None)
            proxy_result = subprocess.run([sys.argv[1], "--connect-proxy"], env=proxy_env, timeout=10)
            proxy.shutdown()
            if proxy.targets != ["127.0.0.1:1", "127.0.0.1:1"]:
                raise RuntimeError("CONNECT regression did not use the loopback-only proxy twice")
        sys.exit(result.returncode or proxy_result.returncode)
