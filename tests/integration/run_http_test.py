#!/usr/bin/env python3
"""End-to-end test of the C++ feed poller against a local HTTPS server.

The server:
  * uses a freshly generated self-signed certificate (the client verifies it)
  * speaks HTTP/1.1 keep-alive, gzip, chunked transfer encoding, ETag/304
  * "publishes" new items after a few polls, like a real wire / EDGAR feed
Usage: run_http_test.py <path-to-stok-it-http>
"""
import gzip
import http.server
import os
import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIX = ROOT / "tests" / "fixtures"

STATE = {"requests": {}, "status": {}, "connections": 0, "lock": threading.Lock()}

RSS_OLD = """<item><guid>old-1</guid><title>Old story already on the wire (NASDAQ: DLTX)</title>
<link>https://example.com/old-1</link><description>old</description><pubDate>Fri, 25 Sep 2026 11:00:00 GMT</pubDate></item>"""
RSS_NEW = """<item><guid>new-1</guid><title>Acme Robotics Signs $20 Million Supply Agreement with Walmart</title>
<link>https://example.com/new-1</link><category>Nasdaq:ACMR</category>
<description>&lt;p&gt;Acme Robotics, Inc. (NASDAQ: ACMR) signed a definitive supply agreement with Walmart Inc. valued at $20 million.&lt;/p&gt;</description>
<pubDate>Fri, 25 Sep 2026 12:00:00 GMT</pubDate></item>"""

EDGAR_OLD = """<entry><title>8-K - DELTA THERAPEUTICS, INC. (0002222222) (Filer)</title>
<link rel="alternate" type="text/html" href="https://localhost:{port}/Archives/old-index.htm"/>
<summary type="html">Item 8.01: Other Events</summary><updated>2026-09-25T07:00:00-04:00</updated>
<id>urn:tag:sec.gov,2008:accession-number=0002222222-26-000001</id></entry>"""
EDGAR_NEW = """<entry><title>8-K - ACME ROBOTICS, INC. (0001234567) (Filer)</title>
<link rel="alternate" type="text/html" href="https://localhost:{port}/Archives/edgar/data/1234567/000123456726000012/0001234567-26-000012-index.htm"/>
<summary type="html"> &lt;b&gt;Filed:&lt;/b&gt; 2026-09-25 &lt;br&gt;Item 1.01: Entry into a Material Definitive Agreement&lt;br&gt;Item 9.01: Financial Statements and Exhibits</summary>
<updated>2026-09-25T08:00:47-04:00</updated>
<id>urn:tag:sec.gov,2008:accession-number=0001234567-26-000012</id></entry>"""


def count(path, status):
    with STATE["lock"]:
        STATE["requests"][path] = STATE["requests"].get(path, 0) + 1
        key = f"{path} {status}"
        STATE["status"][key] = STATE["status"].get(key, 0) + 1
        return STATE["requests"][path]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        with STATE["lock"]:
            STATE["connections"] += 1

    def log_message(self, *args):
        pass

    def send_body(self, body: bytes, ctype="application/xml", etag=None, chunked=False):
        inm = self.headers.get("If-None-Match")
        if etag and inm == etag:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.send_header("Keep-Alive", "timeout=30")
            self.end_headers()
            return 304
        if "gzip" in (self.headers.get("Accept-Encoding") or ""):
            body = gzip.compress(body)
            gz = True
        else:
            gz = False
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        if gz:
            self.send_header("Content-Encoding", "gzip")
        if etag:
            self.send_header("ETag", etag)
        self.send_header("Keep-Alive", "timeout=30")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            step = max(1, len(body) // 5)
            for i in range(0, len(body), step):
                part = body[i:i + step]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        return 200

    def do_GET(self):
        port = self.server.server_address[1]
        path = self.path
        n = STATE["requests"].get(path, 0) + 1
        if path in ("/feed.rss", "/chunked.rss"):
            items = RSS_OLD + (RSS_NEW if n >= 4 else "")
            doc = f'<?xml version="1.0"?><rss version="2.0"><channel><title>t</title>{items}</channel></rss>'
            status = self.send_body(doc.encode(), etag=f'"v{1 if n < 4 else 2}"', chunked=(path == "/chunked.rss"))
        elif path == "/edgar.atom":
            entries = EDGAR_OLD.format(port=port) + (EDGAR_NEW.format(port=port) if n >= 4 else "")
            doc = f'<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom">{entries}</feed>'
            status = self.send_body(doc.encode(), etag=f'"e{1 if n < 4 else 2}"')
        elif path.endswith("-index.htm"):
            html = (FIX / "edgar_index.htm").read_text()
            status = self.send_body(html.encode(), ctype="text/html")
        elif path.endswith("ex99-1.htm"):
            status = self.send_body((FIX / "ex99-1.htm").read_bytes(), ctype="text/html")
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            status = 404
        count(path, status)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length)
        status = self.send_body(b'{"ok":true,"echo_len":%d}' % len(body), ctype="application/json")
        count(self.path, status)


def make_cert(tmp):
    key, crt = os.path.join(tmp, "key.pem"), os.path.join(tmp, "cert.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "1",
         "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return key, crt


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        key, crt = make_cert(tmp)
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(crt, key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        port = srv.server_address[1]
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        env = dict(os.environ)
        for k in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
            env.pop(k, None)
        proc = subprocess.run([binary, str(port), crt, str(FIX)], env=env, capture_output=True, text=True, timeout=60)
        srv.shutdown()
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        print("server:", {k: v for k, v in sorted(STATE["status"].items())})
        print("server: TCP connections accepted:", STATE["connections"])
        ok = proc.returncode == 0
        polls = sum(v for k, v in STATE["status"].items() if k.startswith("/feed.rss") or k.startswith("/edgar.atom"))
        not_modified = sum(v for k, v in STATE["status"].items() if k.endswith(" 304"))
        if not_modified == 0:
            print("FAIL: no conditional GET ever returned 304")
            ok = False
        # Keep-alive: far fewer connections than requests.
        if STATE["connections"] > max(12, polls // 4):
            print(f"FAIL: {STATE['connections']} connections for {polls} polls (keep-alive not working)")
            ok = False
        print("integration:", "PASS" if ok else "FAIL")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
