# RubyFPV support: spike findings

The question this answers: can the app receive RubyFPV video, and what would it
cost? Short version — the receive path is real and works in test, the licence
rules out the approach that made the wfb-ng port trustworthy, and a usable
ground station needs a lot more than a receiver.

## The licence changes the plan

RubyFPV is not under a standard open-source licence. Its `LICENSE` is a
BSD-shaped custom licence with two additional terms that matter:

> * Copyright info and developer info must be preserved as is in the user
>   interface, additions could be made to that info.
> * Military use is not permitted.

The attribution clause is fine — GPL-3 §7(b) explicitly permits that kind of
additional term. **The field-of-use restriction is not.** GPL-3 §7 allows only
an enumerated set of additional restrictions, and "no military use" is not among
them, so upstream's code cannot be linked into this GPL-3.0-only project and
redistributed.

That forecloses the approach used for wfb-ng, where upstream's `rx.cpp` is
compiled straight from the submodule and the interop test drives our receiver
with upstream's own transmitter. **There is no equivalent check here.** Both
sides of `core/tests/test_ruby.cpp` are ours, so it proves the reassembler is
internally sound — not that the format was read correctly.

Nothing from RubyFPV is vendored, copied, or committed. The implementation is
written against the wire format, which is a protocol rather than expression.

### The one thing that did not need reimplementing

Ruby's `code/radio/fec.c` is Luigi Rizzo's Vandermonde GF(2⁸) Reed–Solomon —
the same lineage as wfb-ng's `zfex`, with the same `fec_encode`/`fec_decode`
API shape. So the FEC we already vendor under a compatible licence is the same
algorithm Ruby uses, and the hardest piece to reimplement was already in the
tree. The spike uses `zfex` directly and recovers Ruby-format blocks with it.

## Wire format, as measured

Offsets were taken by compiling upstream's headers in a scratch directory and
printing `offsetof` — measured, not transcribed, so a miscount is not possible.

`t_packet_header` — 24 bytes, little-endian, fronts every Ruby packet:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `uCRC` |
| 4 | 1 | `packet_flags` — component in bits 0–2, headers-only-CRC bit 3, retransmitted bit 4, encryption bit 6, priority bit 7 |
| 5 | 1 | `packet_type` |
| 6 | 4 | `stream_packet_idx` — stream id in the top 4 bits, index in the low 28 |
| 10 | 2 | `packet_flags_extended` |
| 12 | 2 | `total_length` — includes every header and the CRC |
| 14 | 2 | `radio_link_packet_index` |
| 16 | 4 | `vehicle_id_src` |
| 20 | 4 | `vehicle_id_dest` |

`t_packet_header_video_segment` — 36 bytes, follows it on `PACKET_TYPE_VIDEO_DATA`
(22) with component `PACKET_COMPONENT_VIDEO` (1). The fields reassembly needs:
codec in the high nibble of byte 0, `uCurrentBlockIndex` at 17,
`uCurrentBlockPacketIndex` at 21, `uCurrentBlockPacketSize` at 22,
`uCurrentBlockDataPackets` at 24, `uCurrentBlockECPackets` at 25.

The detail worth having got right: **the FEC element is
`uCurrentBlockPacketSize` bytes and begins with the 3-byte
`t_packet_header_video_segment_important`, which is inside the protected region,
not in front of it.** That is what lets a reconstructed element still say how
long its own video is. Every packet in a block carries exactly that many bytes,
parity included.

CRC is standard CRC-32 (reflected 0xEDB88320, init and final xor ~0) over
everything after the field itself. With the headers-only bit set it covers just
the 24-byte header.

Geometry caps at 16 data + 16 EC packets per block.

## What the spike does

`core/src/ruby/ruby_receiver.cpp` — parses the headers, checks CRC, demuxes to
video, reassembles blocks with FEC recovery via `zfex`, and delivers the byte
stream in block order. `core/tests/test_ruby.cpp` covers layout round-trips, the
CRC check value, clean blocks, loss up to the full parity budget, loss beyond it,
corruption, truncation, encrypted packets, and 40 blocks under shuffling with
random loss.

One bug the test caught and the design now avoids: delivery has to be a queue.
A complete block must wait behind an older incomplete one, or the byte stream
reaches the decoder out of order — which looks like corruption rather than a bug.

## What is missing, and it is most of a ground station

A Ruby ground station is a **participant**, not a listener. wfb-ng sprays
FEC-protected packets one way and the receiver decodes whatever lands; Ruby
expects the controller to hold up its end:

- **Pairing** — `PACKET_TYPE_RUBY_PAIRING_REQUEST` (7) from us,
  `PAIRING_CONFIRMATION` (8) back.
- **Clock ping** — `PACKET_TYPE_RUBY_PING_CLOCK` (3) / `..._REPLY` (4).
- **Retransmission requests** — `PACKET_TYPE_VIDEO_REQ_MULTIPLE_PACKETS` (20),
  on a deadline. This is the big one: Ruby's link budget assumes gaps get asked
  for. Without it the link works only as well as its FEC allows, which is worse
  than a stock Ruby ground station on the same hardware — and it fails quietly,
  by being mediocre rather than by breaking.
- **ACKs** where `PACKET_FLAGS_EXTENDED_BIT_REQUIRE_ACK` is set.
- **Link telemetry** so the vehicle's adaptive video has something to adapt to.
- **Encryption** — bit 6, everything after `packet_flags`. Upstream's README
  calls it temporarily disabled; if that changes there is a key exchange too.

The TX plumbing exists (devourer already injects for adaptive-link), so this is
protocol state-machine work rather than new infrastructure. It is still the
majority of the job.

## Where this leaves it

The receiver is a genuine result and cost about a day. The remaining work is
larger than the wfb-ng port was, for a reason that has nothing to do with
protocol difficulty: upstream is a set of Linux processes talking over named
shared memory (`code/base/shared_mem.h`), not a library, so every piece has to
be reimplemented rather than linked — and without an interop test to confirm any
of it.

**The next thing worth doing is not more code.** It is a capture: a few seconds
of real air-unit traffic through `devourer` or `tcpdump`, replayed through this
receiver. That either validates every offset above at once or shows the reading
is wrong, and it costs almost nothing compared to building further on an
unverified foundation.
