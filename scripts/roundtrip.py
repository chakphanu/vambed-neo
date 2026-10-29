#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Codec round trips through a running vambed-neo (or any AMBEd with the
extended protocol), using the dv-testset client library.

For every codec X: PCM -> X, then X -> [every other codec, PCM], then each
of those -> PCM; then xlxd's legacy 19 B opens, checking the reference reply
sizes and decoding every slot. Reports length, level and envelope
correlation against the source, and (with --morse) whether 20 wpm Morse survives PCM -> X -> PCM.
Informational: audio quality is not a goal of vambed-neo.

usage: DVTESTSET=<dv-testset checkout> scripts/roundtrip.py --addr 192.0.2.10 [--morse]
"""
import argparse
import math
import os
import socket
import struct
import sys
import threading
import time

NAMES = {1: "AMBE+", 2: "AMBE+2", 3: "Codec2", 4: "IMBE", 5: "PCM"}


def voice_like(seconds=2.0, f0=140.0, rate=8000):
    """Harmonic bursts (300 ms on, 200 ms off) with a slow pitch glide."""
    out = []
    phase = 0.0
    for n in range(int(seconds * rate)):
        t = n / rate
        on = (t % 0.5) < 0.3
        f = f0 * (1.0 + 0.1 * math.sin(2 * math.pi * 0.7 * t))
        phase += 2 * math.pi * f / rate
        v = sum(math.sin(k * phase) / k for k in range(1, 20) if k * f < 3400)
        out.append(int(5000 * v) if on else 0)
    return struct.pack("<%dh" % len(out), *[max(-32768, min(32767, s)) for s in out])


def frames_of(pcm, size=320):
    pcm += b"\0" * (-len(pcm) % size)
    return [pcm[i:i + size] for i in range(0, len(pcm), size)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", required=True)
    ap.add_argument("--port", type=int, default=10100)
    ap.add_argument("--dvtestset", default=os.environ.get("DVTESTSET", ""))
    ap.add_argument("--morse", action="store_true")
    args = ap.parse_args()
    if args.dvtestset:
        sys.path.insert(0, args.dvtestset)
    from dvtest.audio import score
    from dvtest.proto import ambed

    tx = [0x52540000]

    def run(codec_in, outs, frames, legacy=False):
        """One stream; replies are read while sending, so long inputs do not
        overflow the socket buffer. legacy: 19 B open, raw replies."""
        tx[0] += 1
        ctl = ambed.AmbedClient(args.addr, args.port)
        try:
            if legacy:
                request = ambed.open_legacy("N0TEST", codec_in, *outs)
            else:
                request = ambed.open_ext("N0TEST", codec_in, outs, tx[0])
            std = ambed.parse_std(ctl.rpc(request) or b"")
            if std is None:
                raise RuntimeError("open %d -> %s refused" % (codec_in, outs))
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("0.0.0.0", std.port))
            sock.settimeout(1.0)
            replies = []

            def reader():
                while len(replies) < len(frames):
                    try:
                        replies.append(sock.recvfrom(4096)[0])
                    except socket.timeout:
                        return
            thread = threading.Thread(target=reader)
            thread.start()
            for pid, frame in enumerate(frames):
                sock.sendto(ambed.data_request(codec_in, pid & 0xFF, frame), (args.addr, std.port))
                time.sleep(0.02)
            thread.join()
            sock.close()
            ctl.close_stream(std.stream_id)
        finally:
            ctl.close()
        if len(replies) != len(frames):
            raise RuntimeError("%d of %d replies" % (len(replies), len(frames)))
        if legacy:
            return replies
        out = {c: [] for c in outs}
        for reply in replies:
            for codec, payload in ambed.parse_data_reply(reply, outs)[1]:
                out[codec].append(payload)
        return out

    def report(label, ref, pcm_frames):
        pcm = b"".join(pcm_frames)
        corr, lag = score.envelope_correlation(ref, pcm, max_lag_frames=6)
        level = 20 * math.log10(max(score.rms(pcm), 1e-9) / max(score.rms(ref), 1e-9))
        ok = len(pcm) == len(ref) and score.rms(pcm) > 0
        print("%-28s %5d samples  rms %6.0f (%+5.1f dB vs src)  env corr %.3f lag %+d  %s"
              % (label, len(pcm) // 2, score.rms(pcm), level, corr, lag, "ok" if ok else "FAIL"))
        return ok

    src = voice_like()
    src_frames = frames_of(src)
    all_ok = True
    for x in (1, 2, 3, 4):
        coded = run(5, [x], src_frames)[x]
        others = [c for c in (1, 2, 3, 4) if c != x] + [5]
        fan = run(x, others, coded)
        all_ok &= report("PCM>%s>PCM" % NAMES[x], src, fan[5])
        for y in others[:-1]:
            back = run(y, [5], fan[y])[5]
            all_ok &= report("PCM>%s>%s>PCM" % (NAMES[x], NAMES[y]), src, back)

    # legacy 19 B opens as xlxd sends them; reply slots carry the widths of
    # the reference (AMBE input 9/8/11, Codec2 input 9/9/11, IMBE 9/9/8)
    xlxd_outs = {1: (2, 3, 4), 2: (1, 3, 4), 3: (1, 2, 4), 4: (1, 2, 3)}
    for x, outs in xlxd_outs.items():
        coded = run(5, [x], src_frames)[x]
        replies = run(x, outs, coded, legacy=True)
        widths = [ambed.WIDTH[c] for c in outs]
        size = 1 + sum(1 + w for w in widths)
        good = all(len(r) == size for r in replies)
        slots = {c: [] for c in outs}
        for r in replies:
            off = 2
            for c, w in zip(outs, widths):
                slots[c].append(r[off:off + w])
                off += w + 1
        for c in outs:
            all_ok &= report("legacy %s>%s>PCM (%d B)" % (NAMES[x], NAMES[c], size), src,
                             run(c, [5], slots[c])[5]) and good

    if args.morse:
        from dvtest.audio import morse
        text = "CQ CQ DE N0CALL N0CALL K"
        pcm = morse.encode(text, wpm=20)
        for x in (1, 2, 3, 4):
            coded = run(5, [x], frames_of(pcm))[x]
            back = b"".join(run(x, [5], coded)[5])
            result = morse.check(text, morse.decode(back))
            print("morse 20 wpm PCM>%s>PCM: %s cer %.3f got %r"
                  % (NAMES[x], "exact" if result["exact"] else "differs", result["cer"], result["got"]))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
