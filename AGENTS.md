# AGENTS.md

vambed-neo: public AMBEd-compatible software transcoder (C, GPL-3.0-or-later)
for testing xlxd-style reflectors without DVSI hardware. Flow, not quality.

- Code: `src/vambed_neo.c` (protocol, poll loop), `src/vn_codec.c` (codecs).
- Build: `scripts/build.sh` -> image `vambed-neo:local`; pins in `pins.env`.
- Test: dv-testset wire suite, see README "Testing".
- Rules: SPDX header on every source file; ASCII English only, no emoji.
- No PII: callsigns only N0CALL/N0TEST/XLX999, IPs only 127/8 and 192.0.2/24.
  Run `scripts/pii-check.sh` before every commit (`.local/pii-words` extends it).
- Clean room: protocol from the xlxd ambed (GPL-3.0) and dv-testset docs;
  codecs only through the public APIs of mbelib-neo and codec2. Never copy
  code from closed or differently licensed vocoders.
- No secrets, no `rm -rf`, no `docker prune`; never push.
