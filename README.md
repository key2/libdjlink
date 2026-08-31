# libdjlink

A C11 library that participates in a Pioneer / AlphaTheta **Pro DJ Link** network
as a first-class device and decodes everything the network exposes.

Protocol specification and design rationale: [`../ARCHITECTURE.md`](../ARCHITECTURE.md).

## Status

Implements steps 1–9 of the roadmap in `ARCHITECTURE.md`: the wire codec, device
presence and number negotiation, status/beat ingest, status emission, the
mixer/media control surface, the `dbserver` client, the position tracker, and the
NFS + DeviceSQL + ANLZ metadata path. What remains is the Opus Quad / XDJ-AZ
persona (needs that hardware), Touch Audio PCM, and acting *as* rekordbox.

Verified against live hardware: 2 × `CDJ-3000X` (firmware 1.31) + `DJM-A9`.

| Capability | State |
|---|---|
| Packet classification, all four ports | done, verified |
| Device discovery, roster, expiry | done, verified |
| Device number claim, conflict defense | done, verified |
| CDJ status decode (to 1152-byte CDJ-3000X packets) | done, verified |
| Mixer status, beat, precise position, channels on-air, fader start | done, verified |
| Media slot query + response decode | done, verified |
| Our own keep-alive + CDJ status emission @200 ms | done, verified |
| Sync control, appoint master, load track | built, not hardware-verified |
| **dbserver: folder browse, metadata, waveform** | **done, verified** |
| **dbserver: beat grid, cue list, album art** | **done** (grid verified) |
| **Cue colors (rekordbox LUT), full metadata (label/year/bitrate/orig-artist/remixer)** | **done** (bitrate verified live) |
| **Song structure / phrases (PSSI) + deobfuscation** | **done, verified live over NFS** (13 phrases; dbserver reports the same tag unavailable) |
| **RGB / 3-band waveforms** | **done, verified** (1200-segment 3-band preview, 33867-segment detail) |
| **Playback position interpolation** (`djl_get_position`, events) | **done, verified** |
| **Track signature** (SHA-1) | **done** (KAT-tested) |
| **Auto-fetch metadata on load** (worker + cache + events) | **done, verified** |
| **NFS client (portmap / mount / NFSv2, UTF-16LE)** | **done, verified** on CDJ-3000X USB |
| **DeviceSQL `export.pdb` reader** | **done, verified** (40-track collection, cross-table names) |
| **OneLibrary `exportLibrary.db` reader** (SQLCipher-4 + SQLite) | **done, verified live** (decrypt + read; matches PDB/dbserver) — unlocks OPUS/OMNIS/XDJ-AZ media |
| **ANLZ `.DAT`/`.EXT`/`.2EX` walker** | **done, verified** (grid, cues, phrases, waveforms) |
| **Beat-grid position interpolation** (pre-CDJ-3000 players) | **done, verified** (matches players' own beat numbers) |
| **rekordbox LINK control channel** (7 undocumented 50002 kinds) | **done** (77/77 captured packets decode) |
| **DJM-A9 / V10 mixer state (0x39) + VU meters (0x58) decoders** | **done** (full field/segment decode, unit-tested) |
| **DJM bridge subscription** (0xF9 keepalive + 0x57 subscribe) | **built** to the reference recipe; our DJM-A9 receives it but sends no fader/VU back (see ARCHITECTURE.md 1.12) |
| **Windows / macOS portability** | **Windows verified** under Wine incl. live rig; macOS written, uncompiled |
| Tempo-master handoff dance, beat emission | partial |
| Opus Quad / XDJ-AZ, Touch Audio PCM | not yet (Opus needs the hardware) |
| Acting *as* rekordbox (NFS server + control channel) | not yet |

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

No dependencies beyond libc, pthreads and libm, except the optional OneLibrary
reader, which links **libsqlite3**. The NFS client is on by default and can be
compiled out with `-DDJL_WITH_NFS=OFF`, which leaves a core that needs no sockets
beyond the four DJ Link ports (useful for embedded targets). The OneLibrary
(`exportLibrary.db`) reader is on when libsqlite3 is found; turn it off with
`-DDJL_WITH_ONELIBRARY=OFF`. Its SQLCipher-4 decryption is self-contained, so
only the SQL half needs libsqlite3.

Cross-compiling for Windows works with mingw-w64; link `ws2_32` and `iphlpapi`
(build with `-DDJL_WITH_ONELIBRARY=OFF` unless a Windows libsqlite3 is available).

## Monitor tool

```sh
# passive: never transmits, discovers devices from broadcast traffic only
./build/djl-monitor -i eth0 -o

# join as a virtual CDJ (device 3) and show status + position
./build/djl-monitor -i eth0 -n 3 -s -p

# query every media slot on every player
./build/djl-monitor -i eth0 -n 3 -M
```

## NFS tool — read a player's USB/SD directly

Needs no device number and no `dbserver`, so it works even with four real players
occupying 1–4.

```sh
# list the collection straight out of export.pdb
./build/djl-nfs -i eth0 -p 1 -s 3 -l

# one track, fully: metadata + beat grid + cues + phrases + waveforms
./build/djl-nfs -i eth0 -p 1 -s 3 -t 33

# skip discovery and talk straight to an address; browse the media
./build/djl-nfs -a 169.254.7.185 -s 3 -d PIONEER/rekordbox

# hexdump the first CDJ status packet seen on port 50002
./build/djl-monitor -i eth0 -n 3 -X 0a -P 50002 -C 1
```

Binding ports 50000–50004 needs no privilege (they are above 1024), but
`SO_BINDTODEVICE` is best-effort and may need `CAP_NET_RAW` on some kernels; the
library continues without it.

### Choosing a device number

Metadata over `dbserver` requires a device number in **1–4** that corresponds to
a real, present device. `DJL_NUMBER_LOWEST_FREE` (the default) picks the lowest
free number in 1–4, then 5–6. The library logs a warning if it ends up above 4.

## Library usage

```c
#include <djlink.h>

djl_config cfg;
djl_config_defaults(&cfg);
cfg.interface_name = "eth0";
cfg.device_name    = "myapp";
cfg.send_status    = true;      /* appear as a real player */

djl_context *ctx;
if (djl_context_create(&cfg, &ctx) != DJL_OK) return 1;
if (djl_context_start(ctx) != DJL_OK) return 1;

djl_event ev[32];
for (;;) {
    int n = djl_poll(ctx, ev, 32, 200);
    for (int i = 0; i < n; i++) {
        if (ev[i].kind == DJL_EV_BEAT)
            printf("player %u beat, %.2f BPM\n",
                   ev[i].device, ev[i].u.beat.effective_bpm);
    }
}
djl_context_destroy(ctx);
```

The wire codec is usable standalone with no context, sockets or threads — useful
for offline analysis of `.pcap` captures:

```c
if (djl_wire_classify(50002, buf, len) == DJL_PKT_CDJ_STATUS) {
    djl_cdj_status s;
    if (djl_decode_cdj_status(buf, len, &s) == DJL_OK)
        printf("%s beat %d @ %.2f BPM\n", s.name, s.beat, s.effective_bpm);
}
```

### Protocol research

`djl_set_raw_hook()` delivers every datagram bearing valid Pro DJ Link magic,
including packets the library does not recognize, before any decoding.
`DJL_EV_UNKNOWN_PACKET` reports unrecognized `(port, kind)` combinations. Both
exist so that undocumented traffic stays visible instead of being silently
dropped — this is how the CDJ-3000X findings in `ARCHITECTURE.md` §1.4.1 were
made.

## Design guarantees

- **The wire layer is pure**: no syscalls, no allocation, no globals. Every
  accessor is bounds-checked and total — the fuzz test drives 20 000 random
  inputs through every decoder.
- **No allocation on the packet path.** Fixed receive buffers; snapshots are POD
  copied into caller memory.
- **One I/O thread owns all protocol state**, so the 200 ms status cadence and
  1500 ms keep-alive cannot be stalled by a slow consumer. The event ring is
  bounded and coalesces high-rate events under pressure rather than dropping
  them blindly.
- **Length-agnostic parsing.** Fields are gated on actual packet length, never on
  a model assumption; this is what let the CDJ-3000X's 1152-byte packets decode
  correctly without a code change.

## Tests

`tests/test_wire.c` — 433 assertions covering:

- golden keep-alive vectors captured from real `CDJ-3000X` and `DJM-A9` hardware
- a full 1152-byte `CDJ-3000X` status packet (`tests/vector_cdj3000x.h`)
- two-level `(port, kind)` dispatch, including that `0x06` means *keep-alive* on
  port 50000 but *media response* on 50002, and `0x0a` means *hello* vs *status*
- the two packet framings (device name at `0x0c` vs `0x0b`)
- pitch/BPM/half-frame arithmetic against the values in the specification
- progressive truncation of every real packet — no length may decode "successfully"
- bounds and width rejection on every primitive accessor
- XDR round-trip and bounds, UTF-16LE encoding including surrogate pairs, and RPC
  call/reply framing against denied, program-mismatch and truncated replies
- ANLZ beat-grid, cue and waveform fixtures, including sections whose declared
  lengths lie, plus a synthetic DeviceSQL page with a deleted row
- real captured rekordbox LINK packets as golden vectors for all seven kinds
- beat-grid position tracking: anchoring, jump correction, out-of-order beat
  packets, and the stopped-at-track-end regression
- 50 000 fuzz iterations across all decoders and both file parsers

Run under sanitizers:

```sh
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan -j && ./build-asan/test_wire
```

Clean under ASan+UBSan, including live NFS transfers and 1170 live packets from real
hardware. The suite runs in four configurations: Linux release (851 checks), Linux
ASan/UBSan (851), core with `DJL_WITH_NFS=OFF` (765), and Windows via mingw-w64 (851,
executed under Wine).

## License

Protocol knowledge derives from the [dysentery](https://github.com/Deep-Symmetry/dysentery)
and [beat-link](https://github.com/Deep-Symmetry/beat-link) projects by Deep
Symmetry (Eclipse Public License 2.0). Consider that lineage when choosing a
license for this code.
