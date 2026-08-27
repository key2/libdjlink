# `libdjlink` — Architecture for a Complete C Implementation of Pro DJ Link

**Goal:** a portable C library that covers 100% of the publicly-known Pioneer / AlphaTheta
Pro DJ Link protocol: join the device network as a first-class participant, observe
everything observable, and retrieve every kind of metadata that hardware will surrender.

**Source of truth:** the four reference projects in this tree.

| Project | Language | Role for us |
|---|---|---|
| `dysentery/doc/modules/ROOT/pages/*.adoc` | AsciiDoc | The **normative wire specification**. Byte-level field maps for every known packet. |
| `dysentery/src/dysentery/{vcdj,dbserver,finder}.clj` | Clojure | Minimal reference implementation; hand-written packet templates. |
| `beat-link/src/main/java/...` | Java | The **battle-tested reference implementation** (~30 kLOC). Timings, state machines, edge cases, hardware quirks. |
| `beat-link-trigger`, `beat-carabiner` | Clojure | Consumer-side API surface: what applications actually need (events, Ableton Link, OSC, cues, simulation). |

Not in this tree but required for full metadata coverage: **Crate Digger**
(`org.deepsymmetry:crate-digger`), which supplies the NFS client, the DeviceSQL (`.pdb`)
parser and the ANLZ (`.DAT`/`.EXT`/`.2EX`) parser that `beat-link` depends on. Its file
formats are documented in the `rekordbox-export-analysis` Antora module that
`dysentery` cross-references.

---

## 1. Scope: the full protocol surface

### 1.1 Transport map

| Transport | Port | Direction | Content |
|---|---|---|---|
| UDP broadcast + unicast | **50000** | tx/rx | Device presence, announcement, device-number negotiation, conflict defense |
| UDP broadcast + unicast | **50001** | tx/rx | Beats, precise position, fader start, channels on-air, sync control, master handoff |
| UDP unicast (mostly) | **50002** | tx/rx | CDJ/mixer/rekordbox status, media slot query/response, load track, load settings, Opus metadata |
| UDP unicast | **50004** | tx/rx | Touch Audio (PCM streaming), timing tick |
| TCP | **12523** | tx | `dbserver` port lookup (`RemoteDBServer` probe) |
| TCP | **1051** (dynamic) | tx | `dbserver` metadata/menu/binary queries |
| UDP/TCP | **111** + dynamic | tx | ONC-RPC portmapper → `mount` + `nfs` v2, for pulling `export.pdb` / ANLZ files off player media |

Every UDP packet begins with the 10-byte magic:

```
51 73 70 74 31 57 6d 4a 4f 4c        "Qspt1WmJOL"
```

Byte `0x0a` is the packet **kind**. Kind is only unique *per port* — e.g. `0x0a` is
`DEVICE_HELLO` on 50000 but `CDJ_STATUS` on 50002; `0x06` is `DEVICE_KEEP_ALIVE` on 50000
but `MEDIA_RESPONSE` on 50002. Dispatch **must** be keyed on `(port, kind)`.
(`beat-link/.../Util.java:237` `PACKET_TYPE_MAP` is exactly this two-level map.)

### 1.2 Two packet framings

There are two mutually incompatible layouts after the magic, and getting them confused is
the single most common implementation bug:

**Framing A — "announcement" (port 50000)**

```
0x00..0x09  magic
0x0a        kind
0x0b        subtype byte (usually 0x00)
0x0c..0x1f  device name, 20 bytes, NUL-padded
0x20        0x01
0x21        protocol/structure version (0x02 legacy, 0x03 CDJ-3000-era, 0x04 hello-3000)
0x22..0x23  len_p   = length of the ENTIRE packet
0x24..      payload
```

**Framing B — "status/beat/control" (ports 50001, 50002, 50004)**

```
0x00..0x09  magic
0x0a        kind
0x0b..0x1e  device name, 20 bytes, NUL-padded   <-- starts one byte EARLIER
0x1f        0x01 (0x02 for Load Settings and Precise Position)
0x20        packet subtype (0x00, 0x01, 0x03, ...)
0x21        device number D
0x22..0x23  len_r   = number of bytes REMAINING after this field
0x24..      payload
```

`beat-link` encodes this with `Util.buildPacket()` (header + name = `0x1f` bytes) plus
`Util.setPayloadByte(payload, addr, v)` which does `payload[addr - 0x1f] = v`, so all
code can be written against the *document's* absolute byte addresses.
**Our C API must do the same** — every field constant in our headers uses the absolute
packet address from `dysentery`, and the accessors subtract the header size.

### 1.3 Complete packet catalogue

**Port 50000 (announcement / device numbering)**

| Kind | Name | Notes |
|---|---|---|
| `0x0a` | Device Hello | ×3 @300 ms. `len_p` `0x25`; last byte `0x01` CDJ, `0x02` mixer, `0x05` Stagehand. CDJ-3000 variant: byte `0x21`=`0x04`, `len_p`=`0x26`, trailing `01 40` |
| `0x00` | Number Claim Stage 1 | ×3 @300 ms, carries MAC. `N` counter at `0x24` |
| `0x01` | Number Will Be Assigned | mixer → player on channel-specific jack. Byte `0x0b`=`0x01` |
| `0x02` | Number Claim Stage 2 | ×3 @300 ms, IP `0x24`, MAC `0x28`, `D` `0x2e`, `N` `0x2f`, auto-assign flag `a` `0x31` |
| `0x03` | Number Assignment | mixer → player, `D` at `0x24` |
| `0x04` | Number Claim Stage 3 | ×3 @300 ms (1 if pre-empted). `D` `0x24`, `N` `0x25` |
| `0x05` | Assignment Finished | any settled device → claiming device; `D` = sender's own number |
| `0x06` | Keep-Alive | `D` `0x24`, MAC `0x26`, IP `0x2c`, peer count `p` `0x30`, device type `0x34`, model code `0x35` |
| `0x08` | Device Number In Use | conflict defense; `D` `0x24`, IP `0x25` |

**Port 50001 (beat / control)**

| Kind | Name | Length | Notes |
|---|---|---|---|
| `0x02` | Fader Start | `0x2d` | `C1..C4` at `0x24..0x27`; `00`=play, `01`=stop+cue, `02`=no change |
| `0x03` | Channels On Air | `0x2d` / `0x35` | `F1..F4` at `0x24`; 6-ch variant uses subtype `0x03`, `F5`/`F6` at `0x2e`/`0x2f` |
| `0x0b` | Precise Position | `0x3c` | byte `0x1f`=`0x02`. TrackLength(s) `0x24`, Playhead(ms) `0x28`, Pitch(×100) `0x2c`, BPM(×10) `0x38`. 30 ms cadence |
| `0x26` | Master Handoff Request | `0x28` | `D` at `0x27` |
| `0x27` | Master Handoff Response | `0x2c` | `D` at `0x27`, answer `0x01` at `0x2b` |
| `0x28` | Beat | `0x60` | see §1.4 |
| `0x2a` | Sync Control | `0x2c` | `S` at `0x2b`: `0x10` sync on, `0x20` sync off, `0x01` become master |
| `0x6a` | Beat-cadence heartbeat | `0x35` | AlphaTheta-era; 17-byte zero payload |
| `0x07` | Stagehand transport | `0x38` | action byte `0x2b`, press/release `0x2d` |
| `0x04` | A9 binding-table burst | `0x64` | 5 packets when a CDJ joins (unverified) |

**Port 50002 (status / control / media)**

| Kind | Name | Notes |
|---|---|---|
| `0x05` | Media Query | requester IP `0x24`, `Dr` `0x2b`, `Sr` `0x2f`; `len_r`=`0x0c` |
| `0x06` | Media Response | `len_r`=`0x9c`, total `0xc0` bytes. See §1.5 |
| `0x0a` | CDJ Status | `0xd0`/`0xd4`/`0x11b`/`0x11c`/`0x124`/`0x200` depending on model. See §1.4 |
| `0x10` | rekordbox-Lighting Hello | Opus Quad / XDJ-AZ announce themselves to rekordbox Lighting |
| `0x11` | rekordbox-Lighting status request | we send this when posing as rekordbox |
| `0x19` | Load Track Command | `Dr` `0x28`, `Sr` `0x29`, `Tr` `0x2a`, id `0x2c`, zero-based dest `0x40` |
| `0x1a` | Load Track Ack | |
| `0x29` | Mixer Status | `0x38` bytes; `F` `0x27`, Pitch `0x28`, BPM `0x2e`, `Mh` `0x36`, `Bb` `0x37` |
| `0x34` | Load Settings Command | 116 bytes; byte `0x1f`=`0x02`, no subtype; `D` `0x20`, `Ds` `0x21`, `len_r`=`0x50` |
| `0x39` | DJM-A9 mixer state | 266 B; 24-byte per-channel blocks at `0x24`/`0x3c`/`0x54`/`0x6c` |
| `0x3a` | Stagehand → A9 command | 40 B; opcode `0x20`, arg `0x24` |
| `0x3b` | A9 short heartbeat | 40 B |
| `0x3d` | Track metadata push | 2572 B; only partially decoded |
| `0x55` | Opus / Stagehand data request | 8 opcodes: waveform, cue colors, metadata, PSSI |
| `0x56` | Opus / Stagehand data reply | names itself `VCDJ-3000` on CDJ-3000 |
| `0x58` | A9 VU stream | 584 B @ ~30 Hz |
| `0x68`/`0x69`/`0x6b`/`0x6c`/`0x6d` | Settings-panel snapshot / pref-write | `0x6b` write: on-air-display `0x2c`, quantize `0x3c` |
| `0x40` | `NXS-GW` Kuvo gateway heartbeat | ~every 3 minutes from the elected CDJ-3000 |

**Port 50004 (Touch Audio)**

| Kind | Name | Notes |
|---|---|---|
| `0x1e` | Audio Data | always `0x56c` bytes; `len_p` `0x056c` (with PCM) or `0x002c` (header only); `I` `0x24`, `C` `0x28`, 0x540 B of S16LE 48 kHz stereo |
| `0x1f` | Audio Handover | mixer → initiator player |
| `0x20` | Audio Timing | mixer → player every 7 ms; counter `C` `0x24`, Link-Cue flag `E` `0x28`, elected player `T` `0x29` |

### 1.4 CDJ status and beat field maps

These are the highest-value structures. Our `djl_cdj_status` accessor set must cover all of
them (absolute packet addresses, from `dysentery/.../vcdj.adoc`):

| Addr | Field | Meaning |
|---|---|---|
| `0x21`,`0x24` | `D` | device number |
| `0x27` | `A` | activity: 0 idle, 1 playing/searching/loading |
| `0x28` | `Dr` | device the track was loaded from |
| `0x29` | `Sr` | slot: 0 none, 1 CD, 2 SD, 3 USB, 4 rekordbox collection, 5 ?, 6 streaming direct play, 7 USB2 (XDJ-AZ 4-deck), 8 ?, 9 Beatport LINK |
| `0x2a` | `Tr` | type: 0 none, 1 rekordbox, 2 unanalyzed, 5 audio CD, 6 streaming |
| `0x2c`–`0x2f` | `rekordbox` | track ID (or CD track number, or internal streaming index) |
| `0x32`–`0x33` | `Track` | position in the browsing list |
| `0x35` | `t_srt` | sort mode in effect when loaded |
| `0x37` | `t_src` | menu the track came from (playlist, artist, history, tag list, …) |
| `0x38`,`0x3c` | `t_cat1`,`t_cat2` | menu-path IDs (e.g. artist ID + album ID) |
| `0x46`–`0x47` | `d_n` | number of tracks in the disc/playlist/menu |
| `0x58` | `ld1` | `0x80` pulse on load (nxs2/XDJ-1000) |
| `0x5a`–`0x5b` | `u_c1` | `ffff` pulse on hot-cue / memory-cue change |
| `0x5e`–`0x5f` | `u_t` | pulse on tag-list change |
| `0x66`–`0x67` | `ld2` | `ffff` pulse on load complete |
| `0x6a`,`0x6b` | `Ua`,`Sa` | USB / SD activity blink |
| `0x6f`,`0x73` | `Ul`,`Sl` | USB / SD local mount state (4=absent, 0=mounted, 2/3=unmounting) |
| `0x75` | `L` | link media available anywhere on network |
| `0x7b` | `P1` | play mode (0 no track … `0x12` emergency loop) |
| `0x7c`–`0x7f` | `Firmware` | ASCII |
| `0x84`–`0x87` | `Sync_n` | master-handoff sequence counter |
| `0x89` | `F` | **status bit vector**: b6 Play, b5 Master, b4 Sync, b3 On-Air, b1 BPM-sync-degraded |
| `0x8b` | `P2` | play/stop bitfield; `0x7a`/`0x7e` nexus, `0x6a`/`0x6e` pre-nexus, `0xfa`/`0xfe` nxs2, `0x9a`/`0x9e` XDJ-XZ |
| `0x8c`,`0x98`,`0xc0`,`0xc4` | `Pitch1..4` | `0x100000` = 0 %. 1&3 = effective, 2&4 = local fader |
| `0x90`–`0x91` | `Mv` | tempo validity: `0x8000` rekordbox track, `0x0000` other, `0x7fff` none |
| `0x92`–`0x93` | `BPM` | track BPM ×100 (`ffff` = none) |
| `0x94`,`0x96` | `M_slip`,`BPM_slip` | slip-play mirrors |
| `0x9d` | `P3` | jog/direction mode |
| `0x9e` | `Mm` | 0 not master, 1 master+meaningful, 2 master but non-rekordbox track |
| `0x9f` | `Mh` | master handoff target, else `0xff` |
| `0xa0`–`0xa3` | `Beat` | absolute beat counter (`ffffffff` = unknown) |
| `0xa4`–`0xa5` | `Cue` | beats to next memory point (`0x01ff` = none/far) |
| `0xa6` | `Bb` | beat within bar 1–4 |
| `0xb3` | `u_g` | `ff` pulse on beat-grid edit |
| `0xb7` | `Mp` | CDJ-3000 media presence bits: b0 SD, b1 USB |
| `0xb8`,`0xb9` | `Ue`,`Se` | unsafe eject flags |
| `0xba` | `el` | emergency loop / emergency mode |
| `0xc8`–`0xcb` | `Packet` | packet counter (CDJ-3000 leaves at 0) |
| `0xcc` | `nx` | `0x05` pre-nexus, `0x0f` nexus, `0x1f` XZ / 3000 |
| `0xcd` | `t` | bit 5 set ⇒ Touch Audio capable |
| `0xd0`–`0xef`, `0xff`–`0x10f` | settings blocks | prefixed `12 34 56 78`; `+0x0a` waveform color (1 Blue, 3 RGB, 4 3-Band), `+0x0d` waveform position (1 Center, 2 Left). **Absent on CDJ-3000X fw 1.31 — see §1.4.1** |
| `0x113` | `P4` | undecoded playback bitmask |
| `0x116`,`0x11a` | `T_b`,`T_pos` | high-resolution bar position (only when paused-and-master, or sub-beat loop) |
| `0x11c` | `n_mc` | next memory point index |
| `0x11d`–`0x11f` | `Buf_f`,`Buf_b`,`Buf_s` | forward/back buffer length, fully-buffered flag |
| `0x120`–`0x123` | `NeedleDragPos` | needle marker timestamp |
| `0x158` | `M_t` | Master Tempo on/off |
| `0x15c`–`0x15e` | `Key` | note / minor-major / accidental (`0x64` = out of key) |
| `0x164`–`0x16b` | `KeyShift` | signed 64-bit cents (100 per semitone) |
| `0x1b6`,`0x1be`,`0x1c8` | `Loop_s`,`Loop_e`,`Loop_b` | CDJ-3000 active loop; time = value × 65536 / 1000 ms |

**Beat packet (`0x28`, 96 bytes):**
`nextBeat` `0x24`, `2ndBeat` `0x28`, `nextBar` `0x2c`, `4thBeat` `0x30`, `2ndBar` `0x34`,
`8thBeat` `0x38` (all ms at 0 % pitch; `0xffffffff` if the track ends first),
`0x3c`–`0x53` filled with `0xff`, `Pitch` `0x54`, `BPM` `0x5a`, `Bb` `0x5c`, redundant `D` `0x5f`.

#### 1.4.1 CDJ-3000X firmware 1.31 — measured deviations from the analysis

Captured 2026-08-26 from a live rig of two `CDJ-3000X` players plus a `DJM-A9`,
and preserved as a golden vector in `libdjlink/tests/vector_cdj3000x.h`. These are
first-party observations that do **not** appear in `dysentery`:

| Aspect | Documented | Measured | Consequence for the library |
|---|---|---|---|
| Status packet length | `0x200` max (CDJ-3000) | **`0x480` (1152)** | Never bound-check against a fixed length; `len_r` at `0x22` is authoritative (`0x24 + len_r == total`). |
| Status subtype byte `0x20` | `0x03`–`0x06` | **`0x08`** | Treat `0x20` as opaque; do not gate parsing on known subtypes. Exposed as `djl_cdj_status.subtype`. |
| Settings block | `12 34 56 78` at `0x d0`/`0xff` | **absent** | Probe for the magic; leave settings fields at defaults when missing rather than reading garbage. |
| Keep-alive model code `0x35` | `0x64` CDJ-3000, `0x31` DJM-A9 | **`0xe4`**, **`0xb1`** | High bit set on both, i.e. `flags\|model`. Compare the low 7 bits when identifying a model. |
| Packet counter `0xc8` | fixed `0` on CDJ-3000 | **increments** | Usable as a liveness/ordering hint again. |
| Device name | `CDJ-3000` | **`CDJ-3000X`** | Never match model by exact name string. |
| Mixer device type `0x34` | `02` | **`03`** on DJM-A9 | Accept both as "mixer"; `dysentery`'s Stagehand page predicted this. |

Fields that **did** verify byte-for-byte at their documented offsets on this
hardware: `D`, `A`, `Track`, `t_src`, `d_n`, `U_l`/`S_l`, `P1`, `Firmware`,
`Sync_n`, `F`, `P2`, `Pitch1`–`Pitch4`, `Mv`, `BPM`, `Mm`, `Mh`, `Beat`, `Cue`,
`Bb`, `nx`, `t`, and the whole CDJ-3000 extended region (`M_t`, `Key`,
`KeyShift`, `Loop_s`/`Loop_e`/`Loop_b`).

One behavioural note worth encoding as a test: when a track **ends** (`P1` =
`0x11`), this firmware clears `Dr`, `S_r`, `T_r` and `rekordbox` to zero while
retaining the last `Beat`, `BPM` and `Mv`. A naive "track reference changed"
check therefore fires a spurious load event on every track end. Gate load
detection on a non-zero id *and* a non-`NONE` slot and type.

**Derived quantities** (must be library functions, not caller responsibility):

```
pitch_percent   = (pitch - 0x100000) / (0x100000 / 100.0)
pitch_multiplier= pitch / (double)0x100000
effective_bpm   = (bpm/100.0) * pitch_multiplier
half_frame->ms  = hf * 100 / 15
ms->half_frame  = ms * 15 / 100
```

### 1.5 Media response (`0x06`, 0xc0 bytes)

`Dr` `0x27`, `Sr` `0x2b`, media name UTF-16BE `0x2c` (max `0x40`), creation date UTF-16BE
`0x6c` (max `0x28`), track count `0xa6` (u16), UI color `0xa8`, `Tr` `0xaa`,
My Settings present `0xab`, playlist count `0xae` (u16), total bytes `0xb0` (u64),
free bytes `0xb8` (u64).

**Quirk to encode:** an empty name does *not* mean an empty slot — key emptiness on
`track_count`. Also, CDJ-2000nexus firmware 1.44 never broadcasts type `0x06`
unsolicited; all-in-ones (XDJ-XZ / RX / Opus Quad) never do either. A passive observer
must either actively query, sniff responses between players, or mount the media directly.

### 1.6 `dbserver` (TCP) protocol

1. TCP connect to **12523**, send `00 00 00 0f "RemoteDBServer" 00`, read 2 bytes big-endian
   → the actual dbserver port (1051 in practice).
2. TCP connect to that port, send a 4-byte number field with value 1
   (`11 00 00 00 01`); server echoes it back.
3. Send `SETUP_REQ`: message type `0x0000`, TxID `0xfffffffe`, one number arg = our device
   number. **Must be 1–4** (or ≤6 for CDJ-3000 streaming queries), must correspond to a
   real device present on the network, and must not be the player we are querying.
   Response: `MENU_AVAILABLE` (`0x4000`) whose second arg is *their* device number.
4. Queries. TxID starts at 1 and increments per query; all responses carry the same TxID.
5. `TEARDOWN_REQ` type `0x0100`, TxID `0xfffffffe`.

**Field encoding**

| Tag | Type |
|---|---|
| `0x0f` | u8 |
| `0x10` | u16 BE |
| `0x11` | u32 BE |
| `0x14` | blob: u32 BE length, then bytes |
| `0x26` | string: u32 BE length in UTF-16 code units, then 2×len bytes UTF-16BE, NUL-terminated |

**Message structure**

```
u32 field  0x872349ae          MESSAGE_START
u32 field  TxID
u16 field  message type
u8  field  n = argument count
blob field 12 bytes of argument type tags (t1..t12), 0 beyond n
           tag 0x02 = string, 0x03 = blob, 0x06 = u32
<arguments>
```

**Critical framing rules** (both are historical bugs in `beat-link`):
- Multiple messages may arrive in one TCP segment; one message may span segments.
  Parse by field lengths, never by read boundaries.
- A blob argument declared in the tag list may be **absent** from the body when the
  preceding "length" number field is 0. Never read the blob then. Requests exploit the
  same rule (e.g. `WAVE_PREVIEW_REQ` declares 5 args but sends 4).

**First argument `r:m:s:t`** (a single u32, four independent bytes):
`requesting device` : `menu location` : `slot Sr` : `track type Tr`.
Menu location: `1` main menu, `2` sub-menu, `3` track-info popup, `8` binary/graphical data.
The choice is *not* consistent — waveform detail and ANLZ-tag requests use `1`, while
album art, beat grids and cue lists use `8`.

**Menu paging:** any menu request answered with `MENU_AVAILABLE` is then drained with
`RENDER_MENU_REQ` (`0x3000`, args: `r:m:s:t`, offset, limit, 0, total, 0). Each render
yields `MENU_HEADER` (`0x4001`), *n* × `MENU_ITEM` (`0x4101`), `MENU_FOOTER` (`0x4201`).
**Batch at ≤64** — nexus players fail on larger requests.

**Complete request/response type table** (from `dbserver/Message.java:38`):

| Req | Purpose | Resp |
|---|---|---|
| `0x0000` | setup | `0x4000` |
| `0x0100` | teardown | — |
| `0x1000` | root menu | `0x4000` |
| `0x1001`/`0x1002`/`0x1003`/`0x1004` | genre / artist / album / track menu | `0x4000` |
| `0x1006`/`0x1007`/`0x1008`/`0x100a`/`0x100d` | bpm / rating / year / label / color menu | `0x4000` |
| `0x1010`/`0x1011`/`0x1012`/`0x1013`/`0x1014` | time / bit-rate / history / filename / key menu | `0x4000` |
| `0x1101`/`0x1102`/`0x1103` | artists-for-genre / albums-for-artist / tracks-for-album | `0x4000` |
| `0x1105` | playlist or folder (`id`, `0`=playlist `1`=folder) | `0x4000` |
| `0x1106` | bpm range | `0x4000` |
| `0x1107`/`0x1108`/`0x110a`/`0x110d`/`0x1110`/`0x1111`/`0x1112`/`0x1114` | tracks-for-rating / years-for-decade / artists-for-label / tracks-for-color / tracks-for-time / tracks-for-bitrate / tracks-for-history / key neighbors | `0x4000` |
| `0x1201`/`0x1202`/`0x1206`/`0x1208`/`0x120a`/`0x1214` | two-level filters (`-1` = ALL) | `0x4000` |
| `0x1300` | substring search (uppercase UTF-16) | `0x4000` |
| `0x1301`/`0x130a` | three-level filters | `0x4000` |
| `0x1302`/`0x1402`/`0x1502` | original artist chain | `0x4000` |
| `0x1602`/`0x1702`/`0x1802` | remixer chain | `0x4000` |
| `0x2002` | rekordbox track metadata | `0x4000` → 11 menu items |
| `0x2003` | album art (extra arg `1` = 240×240, `2` = non-rekordbox embedded) | `0x4002` |
| `0x2004` | waveform preview (900 B: 400×2 + 100 B tiny) | `0x4402` |
| `0x2006` | folder menu (raw filesystem) | `0x4000` |
| `0x2104` | cue list (nexus, `0x24`-byte entries) | `0x4702` |
| `0x2202` | unanalyzed / CD metadata | `0x4000` |
| `0x2204` | beat grid (`0x10`-byte entries, **little-endian**) | `0x4602` |
| `0x2904` | waveform detail (1 B/segment, 150/s) | `0x4a02` |
| `0x2b04` | extended cue list (colors, comments, hot cues A–H) | `0x4e02` |
| `0x2c04` | **generic ANLZ tag fetch** — args: id, tag FourCC, file extension | `0x4f02` |
| `0x3000` | render menu | `0x4001`/`0x4101`/`0x4201` |
| — | error / unavailable | `0x0001` / `0x4003` |

**ANLZ tag request constants** (FourCC and extension in *byte-reversed* order,
`dbserver/Message.java:422`):

```
ALNZ_FILE_TYPE_DAT              0x00544144   "DAT"
ALNZ_FILE_TYPE_EXT              0x00545845   "EXT"
ALNZ_FILE_TYPE_2EX              0x00584532   "2EX"
ANLZ_TAG_COLOR_WAVEFORM_PREVIEW 0x34565750   "PWV4"  (EXT)
ANLZ_TAG_COLOR_WAVEFORM_DETAIL  0x35565750   "PWV5"  (EXT)
ANLZ_TAG_3BAND_WAVEFORM_PREVIEW 0x36565750   "PWV6"  (2EX)
ANLZ_TAG_3BAND_WAVEFORM_DETAIL  0x37565750   "PWV7"  (2EX)
ANLZ_TAG_SONG_STRUCTURE         0x49535350   "PSSI"  (EXT)
ANLZ_TAG_CUE_COMMENT            0x324f4350   "PCO2"  (EXT)
ANLZ_TAG_VOCAL_CONFIG           0x43565750   "PWVC"  (2EX)
```

Tag payload starts at offset `0x34` of the returned blob.

### 1.7 Waveform / cue / grid payload formats

| Style | Source | Entry | Layout |
|---|---|---|---|
| Blue preview | `0x2004` | 2 B × 400 | `[0]` height 0–31, `[1]` whiteness 0–7; plus 100 B tiny preview (low nibble = height) |
| Blue detail | `0x2904` | 1 B/segment, 150/s | bits 7–5 color 0–7, bits 4–0 height 0–31 |
| RGB preview | `PWV4` | 6 B × 1200 | u32 `len_entry_bytes`, u32 `len_entries`, then entries. Bytes 0–1 whiteness, 2 low-half energy, 3 low, 4 mid, 5 high |
| RGB detail | `PWV5` | 2 B/segment, 150/s | `rrr ggg bbb hhhhh 00` (big-endian 16-bit) |
| 3-band preview | `PWV6` | 3 B × 1200 | same header as `PWV4`; mid, high, low |
| 3-band detail | `PWV7` | 3 B/segment | header + one extra u32 (`0x00960000` observed); mid, high, low |
| Vocal config | `PWVC` | — | 2 B unknown, then u16 BE `thr_low`, `thr_mid`, `thr_high` |
| Beat grid | `0x2204` | 16 B | **LE** u16 `Bb`, **LE** u16 tempo ×100, **LE** u32 time ms, 8 B unknown. First entry at blob offset `0x14` |
| Cue list (nexus) | `0x2104` | 36 B (`0x24`) | `Fl` loop, `Fc` cue, `H` hot cue 1–3, **LE** u32 `cue`, **LE** u32 `loop`, in 1/150 s |
| Cue list (ext) | `0x2b04` | variable | **LE** u32 `length`, `H` hot cue 1–8 at `0x04`, `Fl` at `0x06` (1 memory, 2 loop), **LE** u32 `cue` `0x0c`, `loop` `0x10`, color-table row `c_id` `0x22`, **LE** u16 `len_c` `0x48`, UTF-16 comment, then at `len_c + 0x4e`: color code `c`, `r`, `g`, `b`. **Entries may be truncated before the comment or color** — bounds-check against `length` |

Rekordbox's 16+ hot-cue color codes map to RGB via a lookup table
(`data/CueList.java:541`, `findRekordboxColor`) — port it verbatim.

**Track signature** (`data/SignatureFinder.java:311`) — SHA-1 over:
title bytes, `0x00`, artist bytes, `0x00`, duration as 4-byte int,
the full RGB waveform-detail data, then for every beat: `beatWithinBar` int and
`timeWithinTrack` int. Requires RGB waveforms specifically.

### 1.8 NFS path (Crate Digger equivalent)

When four real players occupy 1–4, or when we want the *complete* collection, we bypass
`dbserver` and read the player's exported database directly. Players run an ONC-RPC
portmapper plus `mount` v1 and `nfs` v2 servers.

- Mount export path: SD → `/B/`, USB → `/C/` (`data/CrateDigger.java:139`).
- Database: `PIONEER/rekordbox/export.pdb` (DeviceSQL) or
  `PIONEER/rekordbox/exportLibrary.db` (SQLite "Device Library Plus", **encrypted** —
  used by Opus Quad and XDJ-AZ).
- Per-track analysis: the `.DAT` path stored in the database, with the extension swapped
  to `.EXT` and `.2EX`.
- **Quirk:** HFS+-formatted media hides the directory as `.PIONEER`. On a lookup failure
  for `PIONEER`, retry with the dot-prefixed name and remember that per slot.

### 1.9 Opus Quad / XDJ-AZ (rekordbox-Lighting persona)

These units cannot be talked to as a CDJ. Instead we pose as **rekordbox**:

- They broadcast kind `0x10` (rekordbox-Lighting Hello) on 50002.
- We reply/announce with kind `0x11`; they then start sending normal CDJ status packets.
- Kind `0x55` requests / `0x56` replies carry waveforms, cue colors, and **`PSSI`**
  (song structure) data, reassembled across multiple packets.
- Track identity: the Opus reports an internal ID, *not* a rekordbox ID. `beat-link`
  fingerprints the received `PSSI` blob against a user-supplied rekordbox USB export
  archive to recover the real rekordbox ID and slot
  (`OpusProvider.getDeviceSqlRekordboxIdAndSlotNumberFromPssi`).
- Opus exposes device numbers 1, 2 (players) and 33 (mixer).

### 1.10 Stagehand-class devices (device type `0x05`)

Documented in `dysentery/.../stagehand.adoc`. Distinguishing features our parser must
tolerate:

- Abbreviated handshake: only `0x0a`×3 → `0x02`×3, then keep-alives.
- Claims symbolic device number `0x3a` but *operates* with a random number in `0x80`–`0xff`.
- Byte `0x1e` of unicast frames addressed to a Stagehand peer is always `0x3a`.
- Keep-alive byte `0x35` is a **model code**: `0x00` legacy, `0x20` Stagehand,
  `0x31` DJM-A9, `0x64` CDJ-3000. Sending the wrong value can make CDJ-3000s on player
  5/6 kick themselves off the network.
- A9 emits a 100-byte *paired* keep-alive naming both itself and the Stagehand peer.

### 1.11 rekordbox EXPORT / LINK — measured (2026-08-27)

First-party observations from `captures/` (a pcap taken **on** a Windows rekordbox
host, so its own unicast is visible). rekordbox ran as device `0x11`, two
`CDJ-3000X` players (1, 2) and a `DJM-A9` (33) present. These behaviours are not
in the dysentery analysis and are **not yet handled** by the library beyond being
surfaced as `DJL_EV_UNKNOWN_PACKET`.

**Two big structural facts:**

1. **No dbserver (TCP) traffic at all.** When the collection source is rekordbox
   (rather than another CDJ's USB), the players do **not** use the `dbserver`
   TCP protocol. Metadata/art/tracks come over **NFS** plus a set of UDP control
   packets (below). dbserver is the CDJ↔CDJ mechanism; NFS+UDP is the
   rekordbox↔CDJ mechanism.
2. **rekordbox is the NFS *server*; the CDJs are the clients** — the reverse of
   reading a CDJ's USB. The CDJs `GETPORT` (portmap 111) for `mount` (100005)
   and `nfs` (100003) on the rekordbox host, `MNT` its export (path `/`, mount
   status 0 = success), and `LOOKUP` files. **NFS filenames are UTF-16LE**
   (e.g. `LOOKUP "rekordbox_customimage.jpeg"`, `"PIONEER"`), unlike the
   byte-string names used elsewhere. In this capture the visible NFS activity is
   the CDJs pulling rekordbox's custom on-air display image; a track load from
   the collection would additionally `READ` the exported audio and PDB/ANLZ.

**New UDP packet kinds on port 50002** (all carry the standard magic + name;
`body` starts at `0x1f` = `01`, subtype, `Dr`, `len_r`, payload):

| Kind | Dir | Len | Notes |
|---|---|---|---|
| `0x11` | rekordbox→CDJ | 296 | rekordbox announce; payload contains the **host computer name in UTF-16LE** (the "DESKTOP-…" shown when browsing rekordbox on a player). Pairs with the `0x10` hello. |
| `0x16` | rekordbox→CDJ | 48 | small periodic control/keepalive to each player (mostly zero payload) |
| `0x30` | mixer→rekordbox | 36 | DJM-A9 → rekordbox notification |
| `0x31` | rekordbox→mixer | 44 | rekordbox → DJM-A9 (paired with `0x30`) |
| `0x46` | CDJ→rekordbox | 40 | short reply to `0x47` (`…020400a0`) |
| `0x47` | rekordbox→CDJ | 72 | request/config; payload carries the `12 34 56 78` settings marker + an `01`-run — looks like a settings/capability exchange |
| `0x80` | CDJ→rekordbox | 44 | player → rekordbox notification, references rekordbox's device number `0x11` |

**Consequences for the library:**
- To *observe* a rekordbox-linked network fully, classify these seven kinds
  (currently `UNKNOWN`) and decode at least `0x11` (source name) and the
  `0x46/0x47` pair.
- To *act as rekordbox* (serve a collection to players), the library would need
  an **NFS + mount + portmap server** with UTF-16LE filename handling and the
  above UDP control channel — a whole subsystem beyond the NFS *client* planned
  for reading CDJ USBs. This is the single largest capability gap surfaced by
  the capture.

---

## 2. Layered architecture

```
┌───────────────────────────────────────────────────────────────────────────┐
│ L7  Bindings & tools                                                       │
│     djl-monitor CLI · pcap replay harness · OSC bridge · Ableton Link      │
│     bridge · Python/Node FFI · Lua scripting                               │
├───────────────────────────────────────────────────────────────────────────┤
│ L6  Public API                     libdjlink.h  (opaque handles, events)   │
│     djl_context · djl_device_view · djl_track_view · djl_position_view     │
├───────────────────────────────────────────────────────────────────────────┤
│ L5  Derived state / caches                                                 │
│     position tracker (beat + grid + pitch + precise pos)                   │
│     signature · waveform cache · metadata cache · art cache · cue cache    │
├───────────────────────────────────────────────────────────────────────────┤
│ L4  Metadata engines (pluggable providers, ordered fallback)               │
│  ┌─────────────┐ ┌──────────────┐ ┌────────────┐ ┌────────────────────┐   │
│  │ dbserver    │ │ NFS + PDB    │ │ Opus/PSSI  │ │ local archive/cache│   │
│  │ TCP client  │ │ + ANLZ       │ │ matcher    │ │ (offline shows)    │   │
│  └─────────────┘ └──────────────┘ └────────────┘ └────────────────────┘   │
├───────────────────────────────────────────────────────────────────────────┤
│ L3  Protocol engines (one state machine each, all driven by one event loop)│
│     presence & device numbering │ status ingest │ tempo-master & sync      │
│     beat sender │ on-air/fader  │ media slots   │ touch audio             │
├───────────────────────────────────────────────────────────────────────────┤
│ L2  Wire codec (pure, allocation-free, no I/O — 100% unit-testable)        │
│     packet validate/classify · field accessors · packet builders           │
│     dbserver field & message codec · ANLZ/PDB record readers               │
├───────────────────────────────────────────────────────────────────────────┤
│ L1  Platform abstraction (OSAL)                                            │
│     UDP/TCP sockets · interface & MAC enumeration · monotonic clock         │
│     timers · threads/mutex/cond · optional allocator hooks                 │
└───────────────────────────────────────────────────────────────────────────┘
```

### Design invariants

1. **L2 is pure.** Zero syscalls, zero allocation, no globals. Every function takes
   `(const uint8_t *buf, size_t len)` or `(uint8_t *buf, size_t cap)`. This is what makes
   the whole thing testable against the `.pcap` files already in
   `dysentery/doc/assets/captures/`.
2. **One I/O thread, no locks in the fast path.** All four UDP sockets are serviced by a
   single `poll()`/`epoll`/`kqueue` loop with a timer wheel. Protocol state machines run
   on that thread only, so they need no synchronization.
3. **Blocking work never touches the I/O thread.** `dbserver` queries and NFS transfers
   run on a small worker pool; results are handed back via an SPSC queue and applied on
   the I/O thread.
4. **No allocation on the packet path.** Fixed-size ring of receive buffers; derived
   snapshots are POD structs copied into caller-owned memory.
5. **Callbacks are dispatched from a dedicated dispatch thread** (or drained by the
   caller in polling mode) so that a slow user callback can never stall the 200 ms status
   cadence or the beat sender.
6. **Never fail hard on unknown bytes.** Unknown packet kinds, unexpected lengths and
   novel model codes are surfaced as events, logged once, and otherwise ignored.

---

## 3. Module inventory

```
include/
  djlink.h                    umbrella
  djlink/types.h              enums, POD structs, error codes
  djlink/wire.h               L2: codec (usable standalone)
  djlink/context.h            L6: lifecycle, config
  djlink/events.h             L6: event union + subscription
  djlink/device.h             device roster queries
  djlink/status.h             status snapshot accessors
  djlink/sync.h               tempo master, sync, beat sending
  djlink/control.h            load track, load settings, fader start, on-air
  djlink/media.h              slot enumeration & media details
  djlink/metadata.h           track metadata, art, cues, beat grid, waveforms
  djlink/position.h           interpolated playback position
  djlink/touchaudio.h         Touch Audio capture/serve
  djlink/pdb.h                DeviceSQL reader (standalone-usable)
  djlink/anlz.h               ANLZ reader (standalone-usable)

src/
  osal/                       socket_posix.c socket_win32.c iface_*.c clock_*.c thread_*.c
  wire/
    magic.c packet.c          validate, classify, build
    fields_announce.c         port 50000 accessors
    fields_status.c           CDJ/mixer/rekordbox status accessors (§1.4 table)
    fields_beat.c             beat + precise position
    fields_media.c            media response
    fields_touch.c            touch audio
    fields_a9.c               DJM-A9 / Stagehand extensions
    templates.c               all outbound packet templates (§4)
    dbserver_field.c          0x0f/0x10/0x11/0x14/0x26 codec
    dbserver_message.c        message framing + streaming parser
    numeric.c                 pitch, bpm, half-frames, endianness helpers
  net/
    loop.c                    poll/epoll/kqueue + timer wheel
    sockets.c                 four UDP sockets, SO_REUSEADDR, SO_BROADCAST
    interfaces.c             pick interface by observed DJ-Link traffic
  proto/
    presence.c                keep-alive tx, roster, expiry, peer count
    numbering.c               device-number claim state machine (§5)
    status_in.c               ingest 50002
    status_out.c              synthesize our own CDJ status @200 ms
    beat_in.c                 ingest 50001
    beat_out.c                beat sender (drift-corrected)
    master.c                  tempo master handoff state machine (§6)
    mixer.c                   fader start, channels on-air (4 & 6 channel)
    mediaslots.c              query, response, unsolicited broadcasts, mount tracking
    control.c                 load track, load settings
    touchaudio.c              50004
    opus.c                    rekordbox-Lighting persona + PSSI reassembly
  meta/
    provider.c                provider registry + ordered fallback
    dbserver_client.c         connection pool, port lookup, transactions, menu paging
    dbserver_queries.c        one function per KnownType
    nfs/ rpc.c portmap.c mount.c nfs2.c fetcher.c
    pdb/ pdb.c pdb_tables.c   DeviceSQL page/row parser
    anlz/ anlz.c tags_*.c     PQTZ PQT2 PCOB PCO2 PWAV PWV2..PWV7 PWVC PSSI PPTH PVBR
    sqlite_plus.c             optional: Device Library Plus (needs key)
  derive/
    position.c                interpolation (§7)
    signature.c               SHA-1 track signature
    cache.c                   LRU caches keyed by (slot, track_type, id)
  api/
    context.c events.c queries.c
```

**Third-party:** only a SHA-1 implementation is strictly required. SQLite and a
decryption routine are optional (Device Library Plus). No dependency on libpcap in the
library itself — only in the test harness.

---

## 4. Outbound packet templates

Follow `beat-link`'s approach: static byte templates with named offsets, patched in place.
This is far less error-prone than field-by-field serialization and makes byte-exact
comparison against captures trivial.

```c
/* wire/templates.c */
static const uint8_t DJL_TPL_KEEP_ALIVE[0x36] = {
    0x51,0x73,0x70,0x74, 0x31,0x57,0x6d,0x4a, 0x4f,0x4c, 0x06, 0x00,
    /* 0x0c: 20-byte device name */
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0x01, 0x02, 0x00, 0x36,   /* 0x20: 01, proto ver, len_p */
    0x00, 0x01,               /* 0x24: D, first-on-network flag */
    0,0,0,0,0,0,              /* 0x26: MAC */
    0,0,0,0,                  /* 0x2c: IP */
    0x01, 0x00, 0x00, 0x00,   /* 0x30: peer count p */
    0x01, 0x64                /* 0x34: device type, model code */
};
```

Templates required, with the offsets each engine patches:

| Template | Size | Patched |
|---|---|---|
| `HELLO` | `0x25`/`0x26` | name; proto version byte `0x21` |
| `CLAIM_STAGE_1` | `0x2c` | name, `N` `0x24`, MAC `0x26` |
| `CLAIM_STAGE_2` | `0x32` | name, IP `0x24`, MAC `0x28`, `D` `0x2e`, `N` `0x2f`, auto flag `0x31` |
| `CLAIM_STAGE_3` | `0x26` | name, `D` `0x24`, `N` `0x25` |
| `ASSIGNMENT_REQUEST` | `0x32` | byte `0x0b`=`0x01`; IP, MAC, `D`=0, `N`=1, auto flag |
| `NUMBER_IN_USE` | `0x29` | `D` `0x24`, IP `0x25` |
| `KEEP_ALIVE` | `0x36` | `D`, MAC, IP, peer count, device type, model code |
| `CDJ_STATUS` | `0x11c`+ | see below |
| `BEAT` | `0x60` | `D` `0x21`/`0x5f`, six lookahead u32, BPM `0x5a`, `Bb` `0x5c` |
| `PRECISE_POSITION` | `0x3c` | when we act as a CDJ-3000-class source |
| `SYNC_CONTROL` | `0x2c` | `D` `0x21`/`0x27`, `S` `0x2b` |
| `MASTER_HANDOFF_REQUEST` | `0x28` | `D` |
| `MASTER_HANDOFF_RESPONSE` | `0x2c` | `D` |
| `FADER_START` | `0x2d` | `C1..C4` |
| `CHANNELS_ON_AIR` / `_EXT` | `0x2d`/`0x35` | `F1..F4` / `F1..F6` |
| `MEDIA_QUERY` | `0x30` | our IP `0x24`, `Dr` `0x2b`, `Sr` `0x2f` |
| `MEDIA_RESPONSE` | `0xc0` | when we serve media ourselves |
| `LOAD_TRACK` | `0x53` | `Dr` `0x28`, `Sr` `0x29`, `Tr` `0x2a`, id `0x2c`, dest-1 `0x40` |
| `LOAD_SETTINGS` | `0x74` | 27 setting bytes at `0x2c`–`0x4f` |
| `REKORDBOX_LIGHTING_REQUEST` | — | Opus persona |
| `OPUS_PSSI_REQUEST` | — | Opus persona |
| `TOUCH_AUDIO_DATA` | `0x56c` | `I` `0x24`, `C` `0x28`, PCM `0x2c` |

**Our CDJ status packet.** `beat-link`'s `STATUS_PAYLOAD`
(`VirtualCdj.java:2681`) is a `0x11c`-byte template; ours should be the same length so we
present as a nxs2-class player. Fields we patch every 200 ms (payload-relative index =
absolute − `0x1f`):

```
0x21 / 0x24   D
0x27          A            playing ? 1 : 0
0x28          Dr           our own number
0x7b          P1           playing ? 3 : 5
0x84..0x87    Sync_n
0x89          F            0x84 | (play?0x40) | (master?0x20) | (sync?0x10) | (onair?0x08)
0x8b          P2           playing ? 0x7a : 0x7e
0x92..0x93    BPM          round(tempo * 100)
0x9d          P3           playing ? 9 : 1
0x9e          Mm           master ? 1 : 0
0x9f          Mh           handoff target, else 0xff
0xa0..0xa3    Beat
0xa6          Bb
0xc8..0xcb    Packet       monotonic counter
```

Status is **unicast to every known device**, not broadcast (`VirtualCdj.sendStatus`).

**Load-track quirk to implement:** to make an XDJ-XZ load a track, byte `0x20` must be
`0x01` (not `0x02`) and byte `0x4b` must be `0x32`, *and* the sender must appear to be
rekordbox. `beat-link` scans the roster for a real rekordbox instance (device number in
`0x11`–`0x1f`, or rekordbox mobile in `0x29`–`0x2f`) and impersonates it
(`VirtualCdj.java:1992`). The CDJ-3000 ignores the track-type byte and will load a
rekordbox track with the same ID when asked for an unanalyzed one.

---

## 5. Device presence and number negotiation

State machine (`proto/numbering.c`), mirroring `VirtualCdj.java:600–900`:

```
                 ┌──────────┐
                 │  IDLE    │
                 └────┬─────┘
                      │ start()
                      ▼
         ┌────────────────────────┐  observe port 50000 for
         │  WATCHING (4000 ms)    │  SELF_ASSIGNMENT_WATCH_PERIOD,
         └────────┬───────────────┘  build roster, learn taken numbers
                  │
                  ▼
         ┌────────────────────────┐  HELLO ×3 @300 ms
         │  ANNOUNCING            │
         └────────┬───────────────┘
                  │
     ┌────────────┴────────────┐
     │                         │ rx 0x01 (WILL_ASSIGN)
     ▼                         ▼
┌──────────────┐        ┌───────────────────────┐
│ CLAIM_1 ×3   │        │ MIXER_ASSIGNED        │
│ CLAIM_2 ×3   │        │  tx 0x02 subtype 1    │
│ CLAIM_3 ×3   │        │  rx 0x03 → take D     │
└──────┬───────┘        │  tx CLAIM_3 ×1        │
       │                └───────────┬───────────┘
       │ rx 0x08 (IN_USE)           │ rx 0x05 (FINISHED)
       ▼                            │
┌──────────────┐                    │
│ CONFLICT     │───pick next N──────┤
└──────────────┘                    ▼
                          ┌──────────────────────┐
                          │  ONLINE              │
                          │  keep-alive @1500 ms │
                          │  status    @200 ms   │
                          │  defend our number   │
                          └──────────────────────┘
```

Timing constants (all configurable, defaults from `beat-link`):

| Constant | Value | Source |
|---|---|---|
| `DJL_WATCH_PERIOD_MS` | 4000 | `SELF_ASSIGNMENT_WATCH_PERIOD` |
| `DJL_CLAIM_GAP_MS` | 300 | `Thread.sleep(300)` in all four claim loops |
| `DJL_KEEPALIVE_MS` | 1500 | `getAnnounceInterval()` |
| `DJL_STATUS_MS` | 200 | `getStatusInterval()` |
| `DJL_DEVICE_EXPIRY_MS` | 10000 | `DeviceFinder.MAXIMUM_AGE` |
| `DJL_DBSERVER_TIMEOUT_MS` | 10000 | `ConnectionManager.DEFAULT_SOCKET_TIMEOUT` |

**Do not assume 1.5 s for expiry.** Real CDJ-2000nexus units hold a **2.0026 s** median
keep-alive interval; the 10 s expiry window is what makes that safe.

Byte `0x25` of a CDJ keep-alive is *not* a CDJ/mixer discriminator — it records whether
the player was first on the network (`0x02`) or joined an occupied one (`0x01`), latched
for the session. Do not use it for device classification; use byte `0x34` (device type)
and `0x35` (model code).

**Number selection policy** (configurable):
- `DJL_NUMBER_FIXED(n)` — claim exactly *n*, fail on conflict.
- `DJL_NUMBER_AUTO` — set the auto-assign flag, accept mixer assignment.
- `DJL_NUMBER_PREFER_LOW_FREE` — pick the lowest free number in 1–4 (required for
  `dbserver`), else 5–6, else 7+ (observe-only mode).

The library must warn loudly when the chosen number is >6, because `dbserver` queries and
Beatport-LINK metadata will silently return nothing.

**XDJ-XZ hazard:** plugged into its laptop port, the XZ sends `WILL_ASSIGN` for *any*
requested number including 0 and the numbers it is already using, and never defends its
own numbers. Our implementation must always cross-check the assigned number against the
observed roster before accepting it.

---

## 6. Tempo master, sync and beat generation

Handoff protocol (`proto/master.c`, spec in `sync.adoc`):

```
we want master, someone else has it:
   tx MASTER_HANDOFF_REQUEST (0x26) to current master, D = us
   rx MASTER_HANDOFF_RESPONSE (0x27), answer byte == 1
   wait until the outgoing master's status shows Mh == us
   assert Master in our own F and Mm
   outgoing master clears its Master bit, resets Mh to 0xff,
     and sets its Sync_n = max(Sync_n seen) + 1

we hold master and someone requests it:
   tx MASTER_HANDOFF_RESPONSE with answer 1
   keep asserting Master, but set Mh = their number
   when their status asserts Master, clear ours, Mh = 0xff,
     Sync_n = max(Sync_n) + 1

unsolicited handoff:
   if we are master and stopped, and we see a synced+playing device,
     set Mh = that device; it must take over on seeing it
```

Requirements our engine must honour:
- Master role requires device number 1–4 (`beat-link` throws otherwise).
- `Mv` must be `0x8000` for other devices to accept our tempo. If we report anything
  else, mixers display `---.-`.
- When master, we must emit beat packets on the beat. `BeatSender`
  (`BeatSender.java:101`) sleeps until `interval − SLEEP_THRESHOLD` then spins, to keep
  jitter down. In C, use `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` against an
  absolute schedule so error does not accumulate, with the same short busy-wait tail.
- Beat lookahead fields are computed from the beat interval and the beat-within-bar:
  `nextBeat = interval`, `2ndBeat = 2×`, `4thBeat = 4×`, `8thBeat = 8×`,
  `nextBar = interval × (5 − Bb)`, `2ndBar = nextBar + barInterval`
  (`VirtualCdj.java:2206`).

**Sync control we can send:** `0x2a` with `S` = `0x10` (sync on), `0x20` (sync off),
`0x01` (become master). Works to CDJs and DJMs.

---

## 7. Playback position tracking

`derive/position.c`, modelled on `TimeFinder.java`. Three information sources, in
descending order of quality:

1. **Precise Position packets** (`0x0b`, CDJ-3000+, every 30 ms) — absolute playhead in
   ms. Use directly; nothing to interpolate. Survives scratching, reverse, loops and
   needle jumps.
2. **Beat packets + beat grid** — a beat packet gives an exact beat number; the beat grid
   maps beat → ms. Interpolate forward using the pitch and the monotonic clock.
3. **CDJ status packets** (200 ms) — beat number, pitch, play state. Coarser, but the only
   option for pre-3000 players between beats.

Interpolation contract:

```c
/* elapsed = (now_ns - update.ns) / 1e6, scaled by pitch, signed for reverse play */
int64_t djl_position_interpolate(const djl_position_update *u, uint64_t now_ns);
```

Sanity gate, straight from `TimeFinder.java:249`: after interpolating, recompute the beat
at that position from the grid. If it differs from the reported beat by ≥2, discard the
interpolation and snap to the grid — the player jumped or drifted.

Also port:
- `DEFAULT_SLACK` drift tolerance, above which we emit a "position corrected" event.
- `interpolationsDisagree()` — compare the previous and current updates interpolated to
  the same instant; a large skew means the player did something we cannot model, and
  position should be reported as *unknown* rather than wrong.
- Playing/stopped detection combining `P1`, `P2` and `F` — note that holding the jog wheel
  makes `P1` say playing while `P2` says stopped, and that a −100 % Wide-mode pitch stop
  shows as playing in both.

Public snapshot:

```c
typedef struct {
    uint8_t   player;
    bool      valid;
    bool      definitive;       /* from precise position or a beat, not interpolated */
    bool      playing;
    bool      reverse;
    int64_t   position_ms;
    int32_t   beat;             /* -1 if unknown */
    uint8_t   beat_within_bar;  /* 0 if unknown */
    double    pitch;            /* multiplier */
    double    effective_bpm;
    int64_t   track_length_ms;  /* -1 if unknown */
} djl_position;
```

---

## 8. Metadata subsystem

### 8.1 Provider chain

```c
typedef enum {
    DJL_PROVIDER_DBSERVER = 1,   /* TCP; needs device number 1..4 (or <=6) */
    DJL_PROVIDER_NFS      = 2,   /* pull export.pdb + ANLZ; works with 4 players */
    DJL_PROVIDER_OPUS     = 3,   /* rekordbox-Lighting + PSSI fingerprint matching */
    DJL_PROVIDER_ARCHIVE  = 4,   /* user-supplied rekordbox export, offline */
    DJL_PROVIDER_USER     = 5,   /* application-supplied callback */
} djl_provider_kind;

djl_err djl_metadata_set_provider_order(djl_context*, const djl_provider_kind*, size_t);
```

The default order is `NFS, DBSERVER, ARCHIVE, USER` — NFS first because it works
regardless of device number and yields richer data (full collection, all ANLZ tags),
falling back to `dbserver` for streaming tracks and CD audio, which do not exist on disk.
For Opus Quad / XDJ-AZ, `OPUS` is forced first.

A **passive mode** flag suppresses all outbound queries (for read-only sniffing) — the
library then only fills caches from packets it happens to observe.

### 8.2 `dbserver` client

- One connection per player, pooled, idle-reaped. Refcounted so concurrent requests
  share it.
- Per-connection mutex; TxID counter is per-connection.
- Streaming message parser: append bytes to a per-connection buffer, extract complete
  messages by walking field lengths, retain the remainder. Handles both
  many-messages-per-segment and message-spanning-segments.
- Retry with backoff on port lookup (`ConnectionManager.java:304` sleeps `1000 × tries`,
  because a booting player may not be listening yet).
- Every query is a thin wrapper: build `r:m:s:t`, send, await the expected response type,
  translate. Errors `0x0001` / `0x4003` become `DJL_ERR_UNAVAILABLE`, not a hard failure —
  the caller falls through to the next provider.
- **Track type matters.** rekordbox tracks use `0x2002`; unanalyzed and CD tracks use
  `0x2202`; streaming tracks (`Tr` = `0x06`) use `0x2002` again. For streaming tracks,
  `GetBeatGrid`, `GetCueAndLoops` and `GetAdvCueAndLoops` all fail and must not be sent;
  `GetTrackInfo`, and all three waveform styles, succeed. `GetTrackInfo` on a Beatport
  LINK track returns the catalog ID as a pseudo-path like `/26883657.m4a`.
- CDJ-3000 menu items carry extra data in the high 16 bits of the item-type field — mask
  with `0xffff` (`Message.java:1127`).

### 8.3 NFS client

A compact ONC-RPC/XDR implementation is required; no portable C NFS client library is
worth depending on here.

```
rpc.c       XDR encode/decode, RPC call/reply framing (UDP; TCP with record marking)
portmap.c   program 100000 v2, GETPORT for MOUNT (100005) and NFS (100003)
mount.c     MNT / UMNT for "/B/" and "/C/", returns a file handle
nfs2.c      LOOKUP, GETATTR, READ (loop until eof), READDIR
fetcher.c   path resolution, retry/backoff, per-slot handle cache, download to temp file
```

Retry policy from `CrateDigger`: `retryLimit` 3, `RETRY_BACKOFF` 2000 ms,
`MAX_RETRY_INTERVAL` 6000 ms. Named per-file locks prevent two threads from racing to
create and parse the same partially-downloaded file.

Handle caches must be invalidated on media unmount and on device disappearance, or later
reads fail on stale handles.

### 8.4 PDB and ANLZ parsers

`beat-link` delegates these to Crate Digger's Kaitai-generated readers. In C we write
them by hand, as strict bounds-checked readers over an mmap'd file:

- `pdb.c` — DeviceSQL page/table/row structure; tables for tracks, artists, albums,
  genres, labels, keys, colors, playlist tree, playlist entries, artwork, columns.
- `anlz.c` — tag walker over `PMAI`, dispatching to:
  `PQTZ`/`PQT2` beat grid, `PCOB`/`PCO2` cues (basic and extended),
  `PPTH` file path, `PVBR` VBR table,
  `PWAV`/`PWV2` blue preview/tiny, `PWV3` blue detail,
  `PWV4`/`PWV5` RGB, `PWV6`/`PWV7` 3-band, `PWVC` vocal config,
  `PSSI` song structure (phrase analysis).
- Both expose the *same* result structs as the `dbserver` path, so `L5` never has to know
  which provider produced the data.

`PSSI` needs one special note: it is stored obfuscated in some exports and `beat-link`
deobfuscates it before use; the same transform must be implemented.

### 8.5 Result types

```c
typedef struct {
    uint32_t  rekordbox_id;
    djl_slot  slot;
    djl_track_type type;
    char     *title, *artist, *album, *genre, *label, *comment,
             *key, *original_artist, *remixer, *date_added, *file_path;
    uint32_t  artist_id, album_id, genre_id, label_id, key_id, artwork_id;
    uint32_t  duration_s;
    uint32_t  tempo_x100;
    uint8_t   rating;          /* 0..5 */
    djl_color color;           /* id + name + RGB */
    uint32_t  bitrate, year, play_count;
    uint8_t   signature[20];   /* SHA-1, zero until waveform+grid available */
} djl_track_metadata;

typedef struct { uint16_t beat_within_bar; uint16_t tempo_x100; uint32_t time_ms; } djl_beat_grid_entry;

typedef struct {
    bool      is_loop;
    uint8_t   hot_cue;         /* 0 = memory point, 1..8 = A..H */
    uint32_t  start_half_frames, end_half_frames;
    uint8_t   color_id;        /* rekordbox color-table row */
    uint8_t   r, g, b;
    char     *comment;         /* NULL if absent or entry truncated */
} djl_cue_entry;

typedef struct {
    djl_waveform_style style;  /* BLUE / RGB / THREE_BAND */
    bool      is_detail;
    uint32_t  segment_count, bytes_per_segment;
    const uint8_t *data;       /* library-owned, valid until released */
} djl_waveform;
```

Accessors — never raw-byte fiddling in application code:

```c
uint8_t   djl_waveform_height(const djl_waveform*, uint32_t seg, djl_band);
djl_rgb   djl_waveform_color (const djl_waveform*, uint32_t seg);
```

---

## 9. Public API shape

```c
/* ---- lifecycle ---- */
typedef struct djl_context djl_context;

typedef struct {
    const char *interface_name;      /* NULL = auto-detect from DJ-Link traffic */
    const char *device_name;         /* <=20 bytes, default "libdjlink" */
    djl_number_policy number_policy;
    uint8_t     preferred_number;
    djl_device_type advertise_as;    /* CDJ / MIXER / REKORDBOX / OBSERVER */
    uint8_t     model_code;          /* 0x00 legacy, 0x64 CDJ-3000-compatible */
    bool        send_status;         /* false = announce only */
    bool        passive_metadata;
    djl_log_fn  log;  void *log_ud;
    djl_alloc   alloc;               /* optional custom allocator */
} djl_config;

djl_err djl_context_create (const djl_config*, djl_context**);
djl_err djl_context_start  (djl_context*);
void    djl_context_stop   (djl_context*);
void    djl_context_destroy(djl_context*);

/* ---- events ---- */
typedef enum {
    DJL_EV_DEVICE_FOUND, DJL_EV_DEVICE_LOST, DJL_EV_DEVICE_NUMBER_ASSIGNED,
    DJL_EV_DEVICE_NUMBER_CONFLICT,
    DJL_EV_CDJ_STATUS, DJL_EV_MIXER_STATUS, DJL_EV_REKORDBOX_STATUS,
    DJL_EV_BEAT, DJL_EV_PRECISE_POSITION, DJL_EV_TEMPO_CHANGED,
    DJL_EV_MASTER_CHANGED, DJL_EV_MASTER_HANDOFF_STARTED, DJL_EV_SYNC_CHANGED,
    DJL_EV_ON_AIR_CHANGED, DJL_EV_FADER_START,
    DJL_EV_MEDIA_MOUNTED, DJL_EV_MEDIA_UNMOUNTED, DJL_EV_MEDIA_DETAILS,
    DJL_EV_TRACK_LOADED, DJL_EV_TRACK_METADATA, DJL_EV_ALBUM_ART,
    DJL_EV_BEAT_GRID, DJL_EV_CUE_LIST, DJL_EV_WAVEFORM, DJL_EV_ANALYSIS_TAG,
    DJL_EV_SIGNATURE,
    DJL_EV_POSITION,               /* coalesced, rate-limited */
    DJL_EV_SETTINGS_SNAPSHOT,
    DJL_EV_TOUCH_AUDIO_TIMING, DJL_EV_TOUCH_AUDIO_DATA,
    DJL_EV_UNKNOWN_PACKET,         /* forward-compat escape hatch */
} djl_event_kind;

typedef void (*djl_event_fn)(const djl_event*, void *ud);
djl_err djl_subscribe(djl_context*, uint64_t kind_mask, djl_event_fn, void *ud, djl_sub**);

/* Polling alternative for single-threaded / game-loop hosts: */
int djl_poll(djl_context*, djl_event *out, size_t max, int timeout_ms);

/* ---- snapshot queries (thread-safe, copy-out) ---- */
size_t  djl_devices        (djl_context*, djl_device_info *out, size_t max);
djl_err djl_cdj_status     (djl_context*, uint8_t player, djl_cdj_status *out);
djl_err djl_position       (djl_context*, uint8_t player, djl_position *out);
uint8_t djl_tempo_master   (djl_context*);
double  djl_master_tempo   (djl_context*);

/* ---- control ---- */
djl_err djl_set_tempo         (djl_context*, double bpm);
djl_err djl_set_playing       (djl_context*, bool);
djl_err djl_set_synced        (djl_context*, bool);
djl_err djl_become_master     (djl_context*);
djl_err djl_appoint_master    (djl_context*, uint8_t player);
djl_err djl_send_sync         (djl_context*, uint8_t player, bool on);
djl_err djl_send_fader_start  (djl_context*, uint8_t start_mask, uint8_t stop_mask);
djl_err djl_send_on_air       (djl_context*, uint8_t channel_mask /* bits 0..5 */);
djl_err djl_load_track        (djl_context*, uint8_t target,
                               uint8_t src_player, djl_slot, djl_track_type, uint32_t id);
djl_err djl_load_settings     (djl_context*, uint8_t target, const djl_player_settings*);
djl_err djl_query_media       (djl_context*, uint8_t player, djl_slot);

/* ---- metadata (async; results arrive as events, or block with a timeout) ---- */
djl_err djl_request_metadata  (djl_context*, const djl_data_ref*, djl_track_metadata**);
djl_err djl_request_beat_grid (djl_context*, const djl_data_ref*, djl_beat_grid**);
djl_err djl_request_cue_list  (djl_context*, const djl_data_ref*, djl_cue_list**);
djl_err djl_request_waveform  (djl_context*, const djl_data_ref*, djl_waveform_style,
                               bool detail, djl_waveform**);
djl_err djl_request_album_art (djl_context*, const djl_data_ref*, bool hi_res, djl_blob**);
djl_err djl_request_anlz_tag  (djl_context*, const djl_data_ref*,
                               uint32_t fourcc, uint32_t ext, djl_blob**);

/* ---- browsing ---- */
djl_err djl_menu_open (djl_context*, uint8_t player, djl_slot, djl_menu_request*, djl_menu**);
djl_err djl_menu_page (djl_menu*, uint32_t offset, uint32_t limit, djl_menu_item *out, size_t*);
void    djl_menu_close(djl_menu*);
djl_err djl_search    (djl_context*, uint8_t player, djl_slot, const char *utf8, djl_menu**);
```

**Memory ownership:** every `djl_*` object returned by a `djl_request_*` or `djl_*_open`
call is released with a single `djl_release(void*)`. Event payloads are borrowed and
valid only for the duration of the callback; `djl_retain()` promotes one to owned. All
strings are UTF-8, NUL-terminated, converted from UTF-16BE at the codec boundary.

**Player settings** (`djl_player_settings`, 27 fields) map 1:1 to the byte encodings in
`loading_tracks.adoc` and `PlayerSettings.java`; each field is an enum whose values are
the literal protocol bytes (`0x80`/`0x81`/…), so the packet builder is a straight copy.

---

## 10. Threading and concurrency

```
┌───────────────────────────────────────────────┐
│ I/O thread                                    │
│   poll(4 UDP sockets) + timer wheel           │
│   timers: keepalive 1500 ms, status 200 ms,   │
│           beat (tempo-derived), expiry 1000 ms│
│   runs: presence, numbering, status_in/out,   │
│         beat_in/out, master, mixer, media     │
│   owns:  all protocol state (no locks)        │
└──────┬──────────────────────────┬─────────────┘
       │ SPSC queue (results)     │ MPSC queue (events)
       │                          ▼
┌──────┴──────────────┐   ┌──────────────────────┐
│ Worker pool (N=2..4)│   │ Dispatch thread      │
│  dbserver queries   │   │  fans out to         │
│  NFS transfers      │   │  subscribers         │
│  PDB/ANLZ parsing   │   │  (or feeds djl_poll) │
│  SHA-1 signatures   │   └──────────────────────┘
└──────┬──────────────┘
       │ optional: Touch Audio thread (7 ms cadence, elevated priority)
```

Rules:
- Snapshot reads (`djl_cdj_status`, `djl_devices`, …) copy under a short RW lock or, better,
  read a seqlock-protected double buffer published by the I/O thread. No caller ever holds
  a pointer into live protocol state.
- A slow subscriber cannot block the I/O thread. The event queue is bounded; on overflow,
  coalescible events (`DJL_EV_POSITION`, `DJL_EV_CDJ_STATUS`) are replaced in place and a
  drop counter is exposed.
- Touch Audio, if enabled, gets its own thread because 7 ms is tighter than the 200 ms
  protocol cadence the rest of the library is tuned for.

---

## 11. Interface selection and multi-homing

`beat-link` (`Util.findMatchingAddress`, `VirtualCdj.findMatchingAddress`) selects the
interface by watching which one actually receives DJ-Link packets, then extracts its IPv4
address, netmask/prefix, broadcast address and MAC — all of which go into announcement
packets. We must do the same:

1. Enumerate interfaces (`getifaddrs` / `GetAdaptersAddresses`).
2. Bind a probe socket to 50000 on `0.0.0.0` and record the source of the first valid
   DJ-Link packet.
3. Choose the interface whose address is on the same subnet
   (`sameNetwork(prefix, a, b)` — mask comparison).
4. Read the MAC (`SIOCGIFHWADDR` / `getifaddrs` `AF_LINK` / `GetAdaptersAddresses`).
5. Abort with a clear error if two candidate interfaces both see traffic — that is a
   configuration problem the library must not paper over.

Sockets need `SO_REUSEADDR`, `SO_BROADCAST`, and a bind to the *specific* interface
address (not `INADDR_ANY`) so that outbound packets carry the address we advertised.
A Windows note from `beat-link`: `getInterfaceAddresses()` can return null entries; the
enumeration must tolerate that.

---

## 12. Testing strategy

This is the part that makes "100 % of what is known" verifiable rather than aspirational.

### 12.1 Golden-vector codec tests (L2)

`dysentery/doc/assets/captures/` contains labelled hardware captures with `NOTES.md`
describing exactly what happened:

| Capture | Scenario |
|---|---|
| `S01-cold-boot-a`, `S1b-cold-boot-b-alone` | player booting alone |
| `S02-deck-b-joins`, `S2c-deck-a-joins` | joining an occupied network |
| `S04-media-insert`, `S4b-media-insert` | media insert/eject |
| `S05-link-browse` | Link browsing |
| `S06-load-and-play` | load + playback |
| `S13-format-ground-truth` | unicast-visible tap; assignment-finished packet |
| `S15a-sd-alone`, `S15b-sd-and-usb` | media slot combinations |
| `S16a-settings-over-link` | My Settings over Link |
| `S20-browse-ground-truth` | menu/browse ground truth |

Plus `powerup.pcapng`, `to-virtual.pcapng`, `LinkInfo.pcapng`, `LinkInfo2.pcapng`,
`Sync-Master.pcapng` (with a `Sync-Master.txt` transcript).

Test harness: a small pcap reader feeds every UDP payload to
`djl_wire_classify(port, buf, len)` and asserts:
- every packet is recognized (or is on the explicit known-unknown list),
- decoded fields match a checked-in golden JSON produced once and reviewed against the
  `dysentery` field tables,
- **round-trip**: for every packet type we can also *build*, rebuilding from the decoded
  fields reproduces the original bytes exactly.

### 12.2 State-machine replay tests (L3)

Replay a capture into the protocol engines with a virtual clock, and assert the resulting
event sequence. `S13` in particular pins down behaviours that are invisible to broadcast
captures (the unicast assignment-finished packet that truncates the claim series).

Assertions worth encoding as regression tests, all drawn from the `dysentery` field notes:
- booting alone → three stage-3 claim packets; booting onto an occupied network → one;
- keep-alive byte `0x25` latches `0x02`/`0x01` and never changes afterwards;
- `p` (peer count) does change;
- media slots with an empty name but non-zero track count are still reported mounted;
- no unsolicited type `0x06` from CDJ-2000nexus 1.44 — the media engine must fall back to
  active queries.

### 12.3 dbserver / NFS conformance

A mock `dbserver` driven by the message transcripts in `track_metadata.adoc`
(including the annotated REPL session at the end of that file) exercises: setup, metadata,
render-menu paging, absent blobs, split messages, coalesced messages, error responses.
A mock NFS server serves a synthetic export tree, including the `.PIONEER` hidden-folder
case.

### 12.4 Fuzzing

`libFuzzer`/AFL++ harnesses over the pure L2 entry points:
`djl_wire_classify`, each field-extraction function, `dbserver` message parsing, PDB page
parsing, ANLZ tag walking. Seeded from the captures. Run under ASan/UBSan/MSan. Every L2
function must be total: no crash, no read out of bounds, for any input.

### 12.5 Hardware conformance suite

An interactive checklist binary (`djl-conformance`) that walks an operator through the
scenarios that cannot be captured synthetically — become master, hand off master, fader
start, on-air, load track, load settings, Touch Audio — and records pass/fail plus a
capture per scenario. Because our claim is "100 % of what is known", this matrix is the
document that proves it.

### 12.6 Interoperability

Run `libdjlink` and `beat-link` side by side on one network and diff their reported state
per player per 200 ms tick. Any divergence is a bug in one of the two, and finding out
which is exactly the kind of question the golden captures answer.

---

## 13. Build, packaging, portability

- **C11**, no compiler extensions in headers. Warnings as errors
  (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`).
- **CMake** with `find_package(libdjlink)` support, plus a plain `Makefile` for embedded
  users. Static and shared builds. Symbol visibility restricted to `DJL_API`.
- Optional features behind flags: `DJL_WITH_NFS`, `DJL_WITH_PDB`, `DJL_WITH_ANLZ`,
  `DJL_WITH_SQLITE`, `DJL_WITH_TOUCH_AUDIO`, `DJL_WITH_OPUS`. The core (L1–L3 plus
  status/beat/sync) must build with none of them, for microcontroller-class targets.
- Targets: Linux (glibc + musl), macOS, Windows (Winsock2), FreeBSD. Endian-neutral code —
  all multi-byte access goes through explicit big/little-endian helpers, never casts.
- Semantic versioning with a hard rule: the wire codec is versioned independently and
  additively, because new hardware will add fields and must not break existing consumers.

---

## 14. Coverage matrix and known gaps

Implementation status legend: **[done]** implemented and verified against the
live CDJ-3000X / DJM-A9 rig; **[built]** implemented, not yet hardware-verified;
**[todo]** not yet implemented.

| Area | Coverage plan | Status of public knowledge |
|---|---|---|
| Announcement, keep-alive, numbering | **[done]** full, incl. mixer-assigned and CDJ-3000 variants | complete |
| Channel conflict defense | **[done]** defend + re-pick on conflict | complete |
| CDJ status (all models to CDJ-3000X, `0x480` bytes) | **[done]** full field map, length-agnostic | ~90 % of bytes named |
| Mixer status, rekordbox status | **[done]** mixer verified | complete |
| Beat packets, precise position | **[done]** rx verified; **[built]** tx | complete |
| Tempo master handoff, sync control | **[built]** master tracking done; handoff dance pending | complete |
| Fader start, channels on-air (4 & 6 ch) | **[done]** rx verified; **[built]** tx | complete |
| Media query/response, mount tracking | **[done]** round-trip verified | complete |
| Load track, load settings (27 settings) | **[built]** load-track built; settings **[todo]** | complete except mixer settings block |
| `dbserver`: greeting, setup, metadata, folder browse, render/paging | **[done]** verified on CDJ-3000X | complete |
| `dbserver`: search, remaining menu types, cue/beat-grid messages | **[built]**/**[todo]** transport done | complete |
| Waveforms: blue preview + detail | **[done]** verified (CDJ-3000X self-analyzes unanalyzed media) | complete |
| Waveforms: RGB, 3-band (ANLZ tags) | **[built]** framing fixed (was off-by-16); needs rekordbox USB to verify | complete |
| Cues: nexus and extended, colors (rekordbox LUT), comments, hot cues A–H | **[done]** built | complete |
| Beat grids (dbserver) | **[done]** verified on CDJ-3000X | complete |
| Song structure (`PSSI`) phrases + deobfuscation | **[done]** dbserver fetch+parse, unit-tested | complete |
| Vocal config (`PWVC`) | **[todo]** | complete |
| NFS + `export.pdb` + ANLZ | **[todo]** step 9 | complete |
| Device Library Plus (`exportLibrary.db`) | parse if a key is supplied | **encrypted; key not public** |
| Opus Quad / XDJ-AZ via rekordbox-Lighting + PSSI matching | **[todo]** step 10 | mostly complete |
| rekordbox EXPORT NFS *server* (CDJs mount us) + UTF-16LE names | **[todo]** whole subsystem, see §1.11 | observed 2026-08-27 |
| rekordbox link UDP control (`0x11`,`0x16`,`0x30`,`0x31`,`0x46`,`0x47`,`0x80`) | **[todo]** surfaced as unknown; decode `0x11` name first | observed, undocumented |
| Touch Audio (`0x1e`/`0x1f`/`0x20`) | **[built]** timing rx decoded (122k pkts seen); PCM **[todo]** | complete |
| Streaming tracks (Beatport LINK, Cloud Direct Play) | metadata + waveforms; no grid/cues | protocol-limited |
| DJM-A9 mixer state (`0x39`) per-channel | expose decoded fields, raw block for the rest | master/FX block **undecoded** |
| A9 VU stream (`0x58`) | expose raw | **hypothesis only** |
| Stagehand command surface (`0x3a`, `0x6b`, `0x07`) | implement the verified opcodes | partially decoded |
| CDJ-3000 `0x0b` unicast round-robin telemetry | expose raw sub-streams | 8 sub-types, **semantics unknown** |
| `0x3d` track metadata push (2572 B) | decode the ~150 B that is known, expose raw | **mostly undecoded** |
| `0x56` 316-byte reply | expose raw | **likely AES-CBC, key unknown** |
| CDJ → rekordbox unicast on 50000 | capture and expose raw | **not analyzed** |

**Explicit design consequence of that last column:** the library must expose a
`DJL_EV_UNKNOWN_PACKET` event carrying `(port, kind, subtype, length, bytes)` and a raw
accessor for the undecoded regions of packets it *does* understand. That is what turns
`libdjlink` into a research tool as well as a product, and it is how the remaining gaps
get closed — the same way `dysentery` closed the ones we already have.

---

## 15. Implementation order

1. **L1 + L2 + pcap harness.** Codec for every packet in §1.3, golden-vector and
   round-trip tests against all captures. No network I/O yet. This is the largest single
   chunk of value and the easiest to get right.
2. **Presence and numbering.** Join a real network, appear on players' Link screens,
   survive conflicts. Validate against `S01`/`S02`/`S13`.
3. **Status and beat ingest.** Full `djl_cdj_status`, `djl_devices`, beat and precise
   position events. At this point the library is a complete *observer*.
4. **Status and beat emission.** Become a real participant: our own CDJ status at 200 ms,
   beat packets, `Mv` correctness.
5. **Tempo master and sync.** The handoff dance, verified against `Sync-Master.pcapng`.
6. **Mixer features and control.** Fader start, on-air, load track, load settings,
   media query.
7. **`dbserver` client.** Metadata, art, grids, cues, waveforms, menus, search.
8. **Position tracker + signature.** Everything a lighting or video cue engine needs.
9. **NFS + PDB + ANLZ.** Four-player operation and complete-collection access.
10. **Opus Quad / XDJ-AZ.** rekordbox-Lighting persona and PSSI matching.
11. **Touch Audio.**
12. **Raw-research surface.** `DJL_EV_UNKNOWN_PACKET`, A9/Stagehand raw blocks, a
     `djl-dissect` tool in the spirit of `dysentery`'s packet window.

Steps 1–6 give a library that fully participates in the device network and sees
everything the network broadcasts. Steps 7–10 give everything the hardware will tell us
when asked. Steps 11–12 close out the remainder of what is known and provide the hooks
for what is not yet.

---

## 16. References into this tree

| Topic | File |
|---|---|
| Packet type index | `dysentery/doc/modules/ROOT/pages/packets.adoc` |
| Announcement, numbering, keep-alive, CDJ-3000 variants, XDJ-XZ limits | `.../startup.adoc` |
| CDJ / mixer / rekordbox status field maps, settings blocks | `.../vcdj.adoc` |
| Beat packets, precise position, pitch/BPM math | `.../beats.adoc` |
| Tempo master handoff, sync control | `.../sync.adoc` |
| Fader start, channels on-air | `.../mixer_integration.adoc` |
| Media query/response, media broadcasts, `/PIONEER/` layout | `.../media.adoc` |
| Load track, load settings (all 27 settings) | `.../loading_tracks.adoc` |
| `dbserver` protocol, all message types, waveforms, cues, grids, ANLZ tags | `.../track_metadata.adoc` |
| Menu request catalogue, `r:m:s:t`, search | `.../menus.adoc` |
| Touch Audio | `.../touch_audio.adoc` |
| Stagehand, DJM-A9 state, model codes | `.../stagehand.adoc` |
| Open questions | `.../missing.adoc` |
| Packet type ↔ port map, pitch math, half-frames, interface selection | `beat-link/.../Util.java` |
| Numbering state machine, status emission, all outbound templates | `beat-link/.../VirtualCdj.java` |
| Roster, expiry | `beat-link/.../DeviceFinder.java` |
| Port 50001 dispatch | `beat-link/.../BeatFinder.java` |
| Beat emission timing | `beat-link/.../BeatSender.java` |
| Status field accessors and enums | `beat-link/.../CdjStatus.java` |
| Player settings byte encodings | `beat-link/.../PlayerSettings.java` |
| `dbserver` message/field codec, all types | `beat-link/.../dbserver/Message.java`, `Client.java`, `ConnectionManager.java` |
| Menu request catalogue in code | `beat-link/.../data/MenuLoader.java` |
| NFS paths, retry policy, hidden `.PIONEER` | `beat-link/.../data/CrateDigger.java` |
| Opus Quad handling | `beat-link/.../VirtualRekordbox.java`, `data/OpusProvider.java` |
| Waveform styles and requests | `beat-link/.../data/WaveformFinder.java`, `WaveformPreview.java`, `WaveformDetail.java` |
| Cue colors, extended cue parsing | `beat-link/.../data/CueList.java` |
| Position interpolation | `beat-link/.../data/TimeFinder.java` |
| Track signature | `beat-link/.../data/SignatureFinder.java` |
| Reference minimal virtual CDJ (readable templates) | `dysentery/src/dysentery/vcdj.clj` |
| Reference `dbserver` client | `dysentery/src/dysentery/dbserver.clj` |
| Consumer-side API expectations | `beat-link-trigger/src/beat_link_trigger/*.clj` |
| Ableton Link bridging | `beat-carabiner/src/beat_carabiner/core.clj` |
