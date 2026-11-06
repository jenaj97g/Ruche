#!/usr/bin/env python3
"""Stand-in for Discord's local RPC socket (as Discord, or Vesktop's arRPC, provides it).

Answers the handshake with READY and logs every frame Ruche sends as a JSON line.
  fake_discord.py --socket $XDG_RUNTIME_DIR/discord-ipc-0 --log frames.jsonl
"""
import argparse, json, os, socket, struct, sys, threading, time

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", required=True)
    ap.add_argument("--log", required=True)
    a = ap.parse_args()
    os.makedirs(os.path.dirname(a.socket), exist_ok=True)
    if os.path.exists(a.socket):
        os.unlink(a.socket)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(a.socket)
    srv.listen(4)
    log = open(a.log, "a", buffering=1)
    t0 = time.monotonic()
    print("ready", flush=True)

    def serve(c):
        buf = b""
        while True:
            d = c.recv(65536)
            if not d:
                log.write(json.dumps({"t": round(time.monotonic() - t0, 2), "event": "disconnected"}) + "\n")
                return
            buf += d
            while len(buf) >= 8:
                op, n = struct.unpack("<II", buf[:8])
                if len(buf) < 8 + n:
                    break
                body = json.loads(buf[8:8 + n] or b"{}")
                buf = buf[8 + n:]
                log.write(json.dumps({"t": round(time.monotonic() - t0, 2), "op": op, "body": body}, ensure_ascii=False) + "\n")
                if op == 0:
                    ready = {"cmd": "DISPATCH", "evt": "READY", "data": {"v": 1, "user": {"id": "1", "username": "weizkule", "global_name": "weiz"}}}
                    p = json.dumps(ready).encode()
                    c.sendall(struct.pack("<II", 1, len(p)) + p)
                elif op == 1:
                    reply = {"cmd": body.get("cmd"), "nonce": body.get("nonce"), "evt": None, "data": {}}
                    p = json.dumps(reply).encode()
                    c.sendall(struct.pack("<II", 1, len(p)) + p)
                elif op == 2:
                    c.close()
                    return

    while True:
        c, _ = srv.accept()
        threading.Thread(target=serve, args=(c,), daemon=True).start()

if __name__ == "__main__":
    sys.exit(main())
