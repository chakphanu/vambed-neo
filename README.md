<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# vambed-neo

An AMBEd-compatible **software** transcoder for testing xlxd-style
reflectors without DVSI hardware. It speaks the AMBEd UDP protocol (legacy
and extended forms) and serves every codec path with correctly formed
frames, so a reflector and its test suite can exercise every flow:
D-STAR, DMR/YSF, M17, P25 and PCM (USRP/AllStar).

**It is a test tool.** The goal is complete protocol and codec *flow*, not
audio quality. Codecs come from [mbelib-neo](https://github.com/arancormonk/mbelib-neo)
(AMBE+, AMBE+2, IMBE) and [codec2](https://github.com/drowe67/codec2); there
is no DVSI code. Do not expose it to the internet: by default it accepts
control packets from any address (see `--peer`).

## Run

```sh
scripts/build.sh                       # image vambed-neo:local, pins in pins.env
docker run --rm vambed-neo:local --help
docker run -d --name vambed-neo-test --network <reflector network> vambed-neo:local
```

Point the reflector's transcoder address at the container. Stream N uses
UDP base port + N, and the reply goes back to the reflector's IP at that
same port number (as in the reference ambed): put the reflector and
vambed-neo on one docker network (or use host networking) rather than
publishing ports through NAT.

## Options

| Option | Env | Default | Meaning |
|---|---|---|---|
| `-b, --bind IP` | `VAMBED_NEO_BIND` | 0.0.0.0 | listen address |
| `-p, --base-port N` | `VAMBED_NEO_BASE_PORT` | 10100 | control port; stream N uses base+N |
| `-m, --max-streams N` | `VAMBED_NEO_MAX_STREAMS` | 16 | concurrent streams (1..256), then BUSY |
| `-P, --peer IP` | `VAMBED_NEO_PEERS` | any | accept only these peers (repeatable / space separated) |
| `-c, --codecs LIST` | `VAMBED_NEO_CODECS` | 1,2,3,4,5 | codecs offered by CAPS and extended opens |
| `-L, --legacy-only` | `VAMBED_NEO_LEGACY_ONLY=1` | off | behave like a pre-extension ambed: no CAPS reply, 24 B opens ignored |
| `-v, --verbose` | `VAMBED_NEO_VERBOSE=1` | off | log every packet decision |

`--codecs` and `--legacy-only` exist to test the reflector's capability
probe and fallback paths.

## Codecs

| Id | Codec | Frame | Wire format |
|---|---|---|---|
| 1 | AMBE+ 3600x2400 (D-STAR) | 9 B | D-STAR DV voice bytes, FEC and interleave included |
| 2 | AMBE+2 3600x2450 (DMR, YSF) | 9 B | DMR 72-bit AMBE frame, MSB first, DMR dibit interleave |
| 3 | Codec2 3200 (M17) | 8 B | Codec2 frame |
| 4 | IMBE 4400 (P25) | 11 B | 88 voice bits u0..u7, MSB first, no FEC |
| 5 | PCM S16LE 8 kHz (USRP) | 320 B | 160 samples; extended opens only |

Every input is decoded to PCM, then encoded to each requested output, with
per-stream codec state. Each data request gets exactly one reply at once.

Output is deterministic per stream, Codec2 included, so two transcoders
fed the same stream give identical bytes in differential tests (the rig's
reference/SUT compare), whatever each one processed before. codec2 draws
random phases from one process-wide generator (`codec2_rand()`);
vambed-neo defines that function itself to give each stream its own
generator. This needs a libcodec2 that calls it through the PLT, as the
pinned build does: the image build runs `vambed-neo --check-rng` and fails
otherwise, and the startup log states the mode.

## Protocol summary

Control port (UDP 10100):

- `AMBEDPING` + callsign (17 B) -> `AMBEDPONG` (9 B).
- `AMBEDCAPS` (9 B) -> 15 B: version 1, max outputs 4, BE32 codec mask
  (bit n = codec n, 0x3E with every codec).
- Legacy `AMBEDOS` (17/18/19 B: callsign, codec in, 1-3 outputs) ->
  `AMBEDSTD` 14 B (LE16 stream id, LE16 port, codec in, first output) or
  `AMBEDBUSY` 9 B. The combinations served are the reference's with DVSI
  channels: AMBE input with the other AMBE codec as output 1; Codec2/IMBE
  input with AMBE codecs as outputs 1 and 2. Malformed opens get no reply.
- Extended `AMBEDOS` (24 B: callsign, codec in, 4 outputs zero padded,
  BE32 transaction) -> `AMBEDSTD` 18 B or `AMBEDBUSY` 13 B, both ending in
  the transaction. Invalid opens (unknown or duplicate codec, gap, output
  equal to input, no output, invalid callsign, codec outside `--codecs`)
  get BUSY 13. A resend with the same peer IP and port, transaction, input
  and outputs gets the same STD 18; anything else is a new open.
- `AMBEDCS` + LE16 stream id closes a stream (no reply). A stream with no
  data for 3 s is closed too.

Data ports: stream ids run 1..max-streams, so the transcoder uses UDP
base+1 .. base+max-streams (10101-10116 by default) next to the control
port; open them all when firewalling or publishing. A data port accepts
requests only from the IP that opened the stream (and, like the control
port, only from `--peer` addresses when that list is set).

Data port (base + stream id): request `codec_in, pid, frame`; reply
`out1, pid, frame1, out2, frame2, ...` in open order. Legacy replies keep
the reference's fixed three slots: 9/8/11 B for AMBE input, 9/9/11 B for
Codec2 input, 9/9/8 B for IMBE input (32, 33 and 30 B with xlxd's opens).

The full description is in dv-testset's
[docs/protocols/ambed.md](https://github.com/chakphanu/dv-testset/blob/main/docs/protocols/ambed.md).

## Testing with dv-testset

```sh
cd <dv-testset>
TRANSCODER_IMAGE=vambed-neo:local python3 -m pytest -p no:cacheprovider -m wire -v -rs
# optional: DVT_AMBED_STREAM_CAP=4 DVT_TRANSCODER_ARGS="--max-streams 4"
# optional: DVT_AMBED_REF=<ip of a second transcoder> for the legacy compares
```

`scripts/roundtrip.py` (needs `DVTESTSET=<dv-testset checkout>`) runs
PCM -> X -> every other codec -> PCM and the legacy reply layouts against a
running instance and prints level and envelope correlation; `--morse` adds
a 20 wpm Morse check through each codec.

The dv-testset docker rig runs it as the transcoder with
`TRANSCODER_IMAGE=vambed-neo:local scripts/run.sh`; the defaults need no
extra arguments or environment.

## Not mirrored from the reference ambed

- No hardware: every stream is software, the stream limit is `--max-streams`
  (the reference derives it from DVSI channels and answers legacy opens
  with BUSY when it has none).
- No gain stages, AGC or band-pass filter on the PCM path, no DSCP marking.
- Retransmitted extended opens are matched exactly; no other state is kept
  across streams.

## License

GPL-3.0-or-later, see `LICENSE` and `NOTICE` (components, patent notice).
