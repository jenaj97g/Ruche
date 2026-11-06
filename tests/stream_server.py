#!/usr/bin/env python3
"""Local live-radio simulator for testing stream titles.

Serves, from one known title schedule:
  /icy            Icecast-style MP3 with ICY metadata (some titles Latin-1, some UTF-8)
  /extinf/live.m3u8   live HLS, MPEG-TS segments, titles only in #EXTINF
  /packed/live.m3u8   live HLS, packed ADTS segments, ID3 at segment start + exactly at changes
  /tsid3/live.m3u8    live HLS, MPEG-TS segments with an ID3 timed-metadata PID (PES PTS exact)

Every response is logged as JSON lines to --log so a checker can work out which
segment the player joined at.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SR = 44100
FRAME = 1024
SEG = 4.0
WINDOW = 6
PREBUFFER = 3

# (content time in seconds, title, encoding used on the ICY wire)
SCHEDULE = []
_base = [
    ("Artiste Un - Premier Titre", "ascii"),
    ("Céline Dion - Pour que tu m'aimes encore", "latin-1"),
    ("Daft Punk - One More Time", "ascii"),
    ("Indila - Dernière Danse", "utf-8"),
    ("Mylène Farmer - Désenchantée", "latin-1"),
    ("Stromae - Alors on danse", "ascii"),
    ("Zaz - Je veux", "utf-8"),
]
_t = 0.0
for i in range(40):
    title, enc = _base[i % len(_base)]
    if i >= len(_base):
        title = f"{title} #{i // len(_base) + 1}"
    SCHEDULE.append((_t, title, enc))
    _t += [14.5, 12.5, 14.2, 13.8, 12.6][i % 5]


def title_at(t):
    cur = SCHEDULE[0][1]
    for start, title, _ in SCHEDULE:
        if start <= t:
            cur = title
        else:
            break
    return cur


# --------------------------------------------------------------------------- audio

def make_audio(workdir, seconds):
    aac = os.path.join(workdir, "tone.aac")
    mp3 = os.path.join(workdir, "tone.mp3")
    if not os.path.exists(aac):
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        f"sine=frequency=440:sample_rate={SR}:duration={seconds}",
                        "-c:a", "aac", "-b:a", "96k", "-f", "adts", aac], check=True)
    if not os.path.exists(mp3):
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        f"sine=frequency=330:sample_rate={SR}:duration={seconds}",
                        "-c:a", "libmp3lame", "-b:a", "128k", "-write_xing", "0", "-id3v2_version", "0", mp3], check=True)
    return open(aac, "rb").read(), open(mp3, "rb").read()


def adts_frames(data):
    frames = []
    i = 0
    while i + 7 <= len(data):
        if data[i] != 0xFF or (data[i + 1] & 0xF0) != 0xF0:
            raise ValueError("lost ADTS sync at %d" % i)
        length = ((data[i + 3] & 0x03) << 11) | (data[i + 4] << 3) | (data[i + 5] >> 5)
        frames.append(data[i:i + length])
        i += length
    return frames


# --------------------------------------------------------------------------- ID3

def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def id3_frame(fid, data):
    return fid.encode() + syncsafe(len(data)) + b"\x00\x00" + data


def id3_tag(title=None, pts=None):
    frames = b""
    if pts is not None:
        frames += id3_frame("PRIV", b"com.apple.streaming.transportStreamTimestamp\x00" + struct.pack(">Q", pts & 0x1FFFFFFFF))
    if title:
        artist, _, song = title.partition(" - ")
        frames += id3_frame("TIT2", b"\x03" + song.encode("utf-8"))
        frames += id3_frame("TPE1", b"\x03" + artist.encode("utf-8"))
    return b"ID3\x04\x00\x00" + syncsafe(len(frames)) + frames


# --------------------------------------------------------------------------- MPEG-TS

def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


class TsMux:
    AUDIO = 0x101
    META = 0x102
    PMT = 0x1000

    def __init__(self, with_meta):
        self.cc = {}
        self.with_meta = with_meta

    def _packets(self, pid, payload, pcr=None):
        out = b""
        first = True
        while True:
            af = b""
            if first and pcr is not None:
                body = bytes([0x10]) + bytes([(pcr >> 25) & 0xFF, (pcr >> 17) & 0xFF, (pcr >> 9) & 0xFF,
                                              (pcr >> 1) & 0xFF, ((pcr & 1) << 7) | 0x7E, 0x00])
                af = bytes([len(body)]) + body
            room = 184 - len(af)
            if len(payload) < room:
                stuff = room - len(payload)
                if af:
                    af = bytes([af[0] + stuff]) + af[1:] + b"\xff" * stuff
                elif stuff == 1:
                    af = b"\x00"
                else:
                    af = bytes([stuff - 1, 0x00]) + b"\xff" * (stuff - 2)
                room = 184 - len(af)
            chunk, payload = payload[:room], payload[room:]
            cc = self.cc.get(pid, 0)
            self.cc[pid] = (cc + 1) & 0x0F
            out += bytes([0x47, (0x40 if first else 0) | (pid >> 8), pid & 0xFF, (0x30 if af else 0x10) | cc]) + af + chunk
            first = False
            if not payload:
                return out

    def _section(self, table_id, body):
        sec = bytes([table_id, 0xB0 | ((len(body) + 4) >> 8), (len(body) + 4) & 0xFF]) + body
        return b"\x00" + sec + struct.pack(">I", crc32_mpeg(sec))

    def tables(self):
        pat = self._section(0x00, b"\x00\x01\xC1\x00\x00" + struct.pack(">HH", 1, 0xE000 | self.PMT))
        es = bytes([0x0F, 0xE0 | (self.AUDIO >> 8), self.AUDIO & 0xFF, 0xF0, 0x00])
        prog_info = b""
        if self.with_meta:
            # metadata_pointer_descriptor (program level) and metadata_descriptor (stream level), format ID3
            mp = b"\xFF\xFFID3 \xFFID3 \x1F\x00\x01"
            prog_info = bytes([0x25, len(mp)]) + mp
            md = b"\xFF\xFFID3 \xFFID3 \x00\x0F"
            es += bytes([0x15, 0xE0 | (self.META >> 8), self.META & 0xFF, 0xF0, 2 + len(md), 0x26, len(md)]) + md
        body = (b"\x00\x01\xC1\x00\x00" + bytes([0xE0 | (self.AUDIO >> 8), self.AUDIO & 0xFF]) +
                bytes([0xF0 | (len(prog_info) >> 8), len(prog_info) & 0xFF]) + prog_info + es)
        return self._packets(0, pat) + self._packets(self.PMT, self._section(0x02, body))

    @staticmethod
    def pes(stream_id, pts, payload):
        hdr = bytes([0x84 if stream_id == 0xBD else 0x80, 0x80, 5,
                     0x21 | ((pts >> 29) & 0x0E), (pts >> 22) & 0xFF, 0x01 | ((pts >> 14) & 0xFE),
                     (pts >> 7) & 0xFF, 0x01 | ((pts << 1) & 0xFE)])
        length = len(hdr) + len(payload)
        return b"\x00\x00\x01" + bytes([stream_id]) + struct.pack(">H", length if length < 65536 else 0) + hdr + payload

    def audio(self, pts, frames):
        return self._packets(self.AUDIO, self.pes(0xC0, pts, b"".join(frames)), pcr=pts)

    def meta(self, pts, tag):
        return self._packets(self.META, self.pes(0xBD, pts, tag))


# --------------------------------------------------------------------------- HLS segments

class Hls:
    def __init__(self, frames):
        self.frames = frames
        self.fps = SR / FRAME
        per = SEG * self.fps
        self.bounds = []
        i = 0
        k = 0
        while True:
            end = int(round((k + 1) * per))
            if end > len(frames):
                break
            self.bounds.append((i, end))
            i = end
            k += 1
        self.cache = {}
        self.base_pts = 900000  # 10 s, like most encoders start somewhere non-zero

    def nseg(self):
        return len(self.bounds)

    def seg_time(self, n):
        return self.bounds[n][0] / self.fps

    def seg_dur(self, n):
        a, b = self.bounds[n]
        return (b - a) / self.fps

    def frame_pts(self, f):
        return self.base_pts + int(round(f * FRAME * 90000 / SR))

    def changes_in(self, n):
        a, b = self.bounds[n]
        t0, t1 = a / self.fps, b / self.fps
        return [(s, title) for s, title, _ in SCHEDULE if t0 < s < t1]

    def segment(self, kind, n):
        key = (kind, n)
        if key in self.cache:
            return self.cache[key]
        a, b = self.bounds[n]
        start = a / self.fps
        if kind == "packed":
            out = id3_tag(title_at(start), self.frame_pts(a))
            change_frames = {int(round(s * self.fps)): title for s, title in self.changes_in(n)}
            for f in range(a, b):
                if f in change_frames:
                    out += id3_tag(change_frames[f])
                out += self.frames[f]
        else:
            mux = TsMux(with_meta=(kind == "tsid3"))
            out = mux.tables()
            if kind == "tsid3":
                out += mux.meta(self.frame_pts(a), id3_tag(title_at(start)))
            change_frames = {int(round(s * self.fps)): title for s, title in self.changes_in(n)}
            f = a
            while f < b:
                if kind == "tsid3" and f in change_frames:
                    out += mux.meta(self.frame_pts(f), id3_tag(change_frames[f]))
                # one PES per frame keeps the metadata insertion exact
                out += mux.audio(self.frame_pts(f), [self.frames[f]])
                f += 1
        self.cache[key] = out
        return out


# --------------------------------------------------------------------------- server

class State:
    t0 = time.time()
    log = None
    lock = threading.Lock()
    hls = None
    mp3 = b""


def log(**kw):
    kw["t"] = round(time.time() - State.t0, 3)
    with State.lock:
        State.log.write(json.dumps(kw, ensure_ascii=False) + "\n")
        State.log.flush()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send_body(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        parts = path.strip("/").split("/")
        if path == "/icy":
            return self.icy()
        if path == "/schedule.json":
            return self.send_body(json.dumps({"schedule": SCHEDULE, "seg": [State.hls.seg_time(i) for i in range(State.hls.nseg())]}).encode(), "application/json")
        if len(parts) == 2 and parts[0] in ("extinf", "packed", "tsid3"):
            kind, name = parts
            h = State.hls
            avail = int((time.time() - State.t0) / SEG) + PREBUFFER
            avail = min(avail, h.nseg())
            if name == "live.m3u8":
                first = max(0, avail - WINDOW)
                lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:%d" % int(SEG + 0.999),
                         "#EXT-X-MEDIA-SEQUENCE:%d" % first]
                ext = "aac" if kind == "packed" else "ts"
                for i in range(first, avail):
                    title = title_at(h.seg_time(i)) if kind == "extinf" else ""
                    lines.append("#EXTINF:%.5f,%s" % (h.seg_dur(i), title))
                    lines.append("seg%d.%s" % (i, ext))
                log(kind=kind, req="playlist", first=first, last=avail - 1)
                return self.send_body(("\n".join(lines) + "\n").encode("utf-8"), "application/vnd.apple.mpegurl")
            if name.startswith("seg"):
                n = int(name[3:].split(".")[0])
                log(kind=kind, req="segment", seq=n, start=h.seg_time(n))
                return self.send_body(h.segment(kind, n), "audio/aac" if kind == "packed" else "video/mp2t")
        self.send_error(404)

    def icy(self):
        want_meta = self.headers.get("Icy-MetaData") == "1"
        metaint = 16000  # 128 kbit/s -> one metadata block per second
        self.protocol_version = "HTTP/1.0"
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("icy-name", "Ruche Test FM")
        self.send_header("icy-br", "128")
        if want_meta:
            self.send_header("icy-metaint", str(metaint))
        self.end_headers()
        sent = 0
        start = time.time()
        burst = 2 * metaint  # 2 s initial burst, as Icecast does
        last_title = None
        conn = id(self)
        log(kind="icy", req="connect", meta=want_meta, conn=conn)
        try:
            while True:
                pos = sent % len(State.mp3)
                chunk = (State.mp3 + State.mp3)[pos:pos + metaint]
                self.wfile.write(chunk)
                sent += metaint
                if want_meta:
                    t = sent / metaint  # content seconds after this block
                    # the title in effect for the audio that follows
                    title, enc = next(((ti, e) for s, ti, e in reversed(SCHEDULE) if s <= t), (SCHEDULE[0][1], "ascii"))
                    if title != last_title:
                        raw = ("StreamTitle='%s';" % title).encode(enc)
                        raw += b"\x00" * ((16 - len(raw) % 16) % 16)
                        self.wfile.write(bytes([len(raw) // 16]) + raw)
                        if last_title is not None or True:
                            log(kind="icy", req="title", at=t, title=title, enc=enc, conn=conn)
                        last_title = title
                    else:
                        self.wfile.write(b"\x00")
                self.wfile.flush()
                ahead = sent / 16000.0 - (time.time() - start) - burst / 16000.0
                if ahead > 0:
                    time.sleep(ahead)
        except (BrokenPipeError, ConnectionResetError):
            log(kind="icy", req="disconnect", conn=conn, sent=sent)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--dir", default=".")
    ap.add_argument("--log", default="server.jsonl")
    a = ap.parse_args()
    aac, mp3 = make_audio(a.dir, 300)
    State.hls = Hls(adts_frames(aac))
    State.mp3 = mp3
    State.log = open(a.log, "w")
    State.t0 = time.time()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.daemon_threads = True
    print("ready", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
