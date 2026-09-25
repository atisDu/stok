#!/usr/bin/env python3
"""Offline end-to-end demo of stokd (no internet or market-data subscription).

1. Starts a local HTTPS server that behaves like a press-release wire and the
   EDGAR "latest filings" feed (self-signed cert, keep-alive, gzip, ETag/304).
   After a few polls it "publishes" an Acme Robotics contract story.
2. Starts stokd with market.source = bridge and feeds pointing at that server.
3. Streams quotes and trades into the bridge: a pre-news print, then a +22%
   run on volume with a tight spread, a further run that hits the paper
   trader's target, then an LULD pause.
4. Prints what stokd emitted (WATCH -> ALERT/HIGH -> HALT), the paper trade,
   and the stok-report summary of the journal.

Usage: python3 scripts/demo.py [build_dir]
"""
import http.server
import json
import os
import pathlib
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
import run_http_test as srvmod  # noqa: E402  (reuses the test server)

BUILD = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build")


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    stokd = BUILD / "stokd"
    if not stokd.exists():
        print(f"build first: {stokd} not found")
        return 1
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stok-demo-"))
    key, crt = srvmod.make_cert(str(tmp))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), srvmod.Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    # Reference data: the fixture security master + share counts + baseline.
    ref = tmp / "data" / "ref"
    base = tmp / "data" / "baseline"
    ref.mkdir(parents=True)
    base.mkdir(parents=True)
    for f in ("nasdaqlisted.txt", "otherlisted.txt", "company_tickers_exchange.json"):
        shutil.copy(ROOT / "tests" / "fixtures" / f, ref / f)
    (ref / "fundamentals.tsv").write_text(
        "cik\tshares_outstanding\tshares_asof\tpublic_float_usd\tfloat_asof\n1234567\t12000000\t2026-08-01\t0\t\n")
    (base / "baseline.tsv").write_text("ticker\tprev_close\tadv\tdays\tasof\nACMR\t2.00\t30000\t20\t2026-09-24\n")

    bridge_port = free_udp_port()
    conf = tmp / "demo.conf"
    conf.write_text(f"""
[general]
data_dir = {tmp}/data
rules_file = {ROOT}/config/rules.tsv
log_level = info
[net]
user_agent = stok-demo demo@example.com
ca_file = {crt}
stats_interval_s = 0
[host localhost]
max_rps = 50
[feed wire]
kind = rss
url = https://localhost:{port}/feed.rss
interval_ms = 250
lanes = 2
[feed edgar_8k]
kind = edgar
url = https://localhost:{port}/edgar.atom
interval_ms = 250
lanes = 2
[signal]
push_watch = true
eval_interval_us = 1000
stats_interval_s = 3600
[movers]
enabled = false
[paper]
enabled = true
latency_ms = 50
slippage_bps = 20
position_usd = 1000
[market]
source = bridge
bridge_port = {bridge_port}
write_baseline = false
[alerts]
stdout = true
""")
    env = {k: v for k, v in os.environ.items() if k.lower() not in ("https_proxy", "http_proxy")}
    proc = subprocess.Popen([str(stokd), "-c", str(conf)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send(line):
        udp.sendto(line.encode(), ("127.0.0.1", bridge_port))

    time.sleep(0.4)
    send("Q ACMR 1.99 3000 2.01 2500")
    send("T ACMR 2.00 1000")          # last print before the news
    time.sleep(1.6)                   # the server publishes the story on the 4th poll
    send("Q ACMR 2.19 5000 2.21 4000")
    send("T ACMR 2.20 50000")
    time.sleep(0.05)
    send("T ACMR 2.45 150000")        # +22.5% since the news on ~$480K
    send("Q ACMR 2.44 5000 2.46 3000")  # 0.8% spread: tradeable
    time.sleep(0.4)                   # paper order arrives after 50 ms and fills at the ask
    send("Q ACMR 3.00 5000 3.02 2000")
    send("T ACMR 3.01 20000")         # bid +22% over the entry: paper target
    time.sleep(0.4)
    send("H ACMR H LUDP")             # volatility pause
    time.sleep(0.8)
    proc.send_signal(signal.SIGINT)
    out, err = proc.communicate(timeout=20)
    srv.shutdown()

    print("=" * 78)
    print("stokd stdout (alerts):")
    print("=" * 78)
    print(out)
    print("=" * 78)
    print("stokd log (excerpt):")
    print("=" * 78)
    for line in err.splitlines():
        if any(k in line for k in ("primed", "running", "reference data", "scorer", "final", "req=", "items=", "engine:",
                                   "paper")):
            print(line)
    journal = sorted((tmp / "data" / "journal").glob("*/*.jsonl"))
    print("=" * 78)
    print("journal files:", ", ".join(f"{p.parent.name}/{p.name} ({sum(1 for _ in open(p))} lines)" for p in journal))
    sig = [p for p in journal if p.name == "signals.jsonl"]
    if sig:
        for line in open(sig[0]):
            j = json.loads(line)
            print(f"  {j['tier']:6} {j['ticker']:6} score={j['score']} move={j.get('move_pct')}% "
                  f"news->signal={j.get('news_to_signal_ms', '-')} ms  why={j['why']}")
    trades = [p for p in journal if p.name == "trades.jsonl"]
    closed = []
    if trades:
        for line in open(trades[0]):
            j = json.loads(line)
            if j["event"] == "close":
                closed.append(j)
                print(f"  paper {j['ticker']}: bought {j['shares']:.0f} @ {j['entry_px']} "
                      f"sold @ {j['exit_px']} ({j['exit_reason']}) pnl ${j['pnl_usd']} ({j['pnl_pct']:+.2f}%)")
    report = subprocess.run([str(BUILD / "stok-report"), "-d", str(tmp / "data" / "journal")], capture_output=True,
                            text=True)
    print("=" * 78)
    print("stok-report:")
    print("=" * 78)
    print(report.stdout)
    ok = ("WATCH ACMR" in out) and ("ALERT ACMR" in out or "HIGH ACMR" in out) and ("HALT ACMR" in out) \
        and len(closed) == 1 and closed[0]["exit_reason"] == "target" and report.returncode == 0
    print("demo:", "PASS" if ok else "FAIL")
    shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
