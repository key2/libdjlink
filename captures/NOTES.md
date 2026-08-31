# Stagehand device gate — stagehand-reference-noresponse-20260831.pcap

- **When:** 2026-08-31, ~24 s.
- **Interface / rig:** as below (DJM-A9 `169.254.116.4` dev 33, two CDJ-3000X).
- **What ran:** the **reference `alphatheta-connect`** library (chrisle) in
  `connectMethod: 'stagehand'` mode — i.e. the exact third-party implementation
  the community reports unlocks DJM fader/VU. It joined the network, saw the A9
  (id 33) and both CDJs, and logged `mixerState=0 vu=0` for the whole run.
- **What the A9 sent (this file):** only its normal broadcasts — `0x03` on-air
  (75×) and `0x06` keep-alive (11×). **Zero `0x39`, zero `0x58`, zero unicast**
  to the Stagehand peer, and no ARP for the peer's IP.

**Conclusion.** libdjlink's Stagehand persona is byte-for-byte identical on the
wire to this reference (verified live: `0x0a`/`0x02`/`0x06` with device type
`0x05`, model `0x20`, proto marker `0x03`, symbolic `0x3a`, runtime number
141–211, AlphaTheta-OUI MAC). Steady-state, **both** implementations elicit no
`0x39`/`0x58` from this A9 (this pcap is the reference run). But two further live
observations refine that:

- The **CDJ-3000X reliably serves the persona** — it unicasts its Stagehand
  monitor stream (~30/s `0x0b` telemetry + ~4/s `0x0a` status) to us the moment
  we are online. So AlphaTheta players do accept the persona.
- The A9 **did** push real `0x39` fader-status once (57 packets, plausible
  values matching this unit) in a single unreproducible window, then stopped.

So the persona is correct and accepted; the A9's `0x39`/`0x58` push is gated and
intermittent on this rig — a device-side condition (Utility/`PRO DJ LINK` setting,
firmware, or an election state), not our code. The same A9 also ignored the
`0xF9` bridge persona (§1.12). Full analysis in ARCHITECTURE.md §1.14.

# Reference capture — reference-20260826-234257.pcap

- **When:** 2026-08-26 23:42:57 local, ~8 min (requested 3 min; the `timeout`
  wrapper was orphaned so it ran longer, then stopped as root).
- **Interface:** `enx00e04c680292` (Realtek USB-Ethernet, `00:e0:4c:68:02:92`),
  our host at `169.254.241.203/16`.
- **Rig:** DJM-A9 (`169.254.116.4`, dev 33), two CDJ-3000X players
  (`169.254.7.185` dev 1, `169.254.7.162` dev 2).
- **Operator actions during capture:** powered a machine on, inserted a USB
  stick, loaded and played a track from the other CDJ-3000X.
- **Size:** 3578 packets / ~497 KB.

## Topology caveat (important)

Our host is on the **same switched L2 segment** as the players but the switch is
**not** mirroring the players' ports to us. A switch only forwards unicast to the
destination port, so a passive capture here sees **only broadcast/multicast**.

Evidence in this file:

- IP endpoints: DJM-A9, both CDJs all show **Tx > 0, Rx 0** — they only
  transmitted to the broadcast address as far as we can see.
- `0` packets addressed to `169.254.7.185` or `169.254.7.162` (no unicast).
- `0` TCP packets, `0` RPC/NFS/portmap/mount packets.
- `0` packets on port 50002 (status and media response are unicast).

## What the capture DOES contain (broadcast plane)

Validated against `dysentery/.../startup.adoc` — the boot sequence is textbook:

| t (rel) | Source | Event |
|---|---|---|
| 26–33 s | all | DHCP discover/APIPA self-assign (link-local `169.254.x`) |
| 36.08–36.68 s | DJM-A9 | `0x0a` Device Hello ×3 at 300 ms |
| 36.98–37.58 s | DJM-A9 | `0x00` Number Claim Stage 1 ×3 |
| 37.88–38.48 s | DJM-A9 | `0x02` Number Claim Stage 2 ×3 |
| 38.78–39.38 s | DJM-A9 | `0x04` Number Claim Stage 3 ×3 |
| 55.5–59.7 s | both CDJs | full `0x0a`→`0x00`→`0x02`→`0x04` claim, interleaved |
| 39.9–517.7 s | players | `0x28` beat / `0x0b` precise position / `0x03` on-air (port 50001, broadcast) |
| throughout | all | `0x06` keep-alive on 50000 (~2 s cadence: 240 mixer, 219+219 players) |

Port breakdown: `50001`=2295, `50000`=710 (of which `0x06`=678), plus DHCP(67),
SSDP(1900), mDNS(5353), and unrelated host noise.

The 300 ms claim cadence and 2 s keep-alive interval are directly measurable
here and match the library's timing constants.

## What the capture does NOT contain, and why it can't

The question that motivated this — **how one CDJ-3000X reads files from another's
USB** — happens entirely over **unicast** and is therefore absent:

- **dbserver** (TCP 12523 → assigned port) — menu/folder browse and metadata.
- **NFS** (portmap 111 → mountd → nfs 2049, UDP) — the actual file bytes.
- **CDJ/mixer status** and **media response** (UDP 50002) — unicast to the
  requester.

All of these are player↔player unicast, switched away from our port.

## To capture the unicast (any one of these)

1. **Port mirroring / SPAN** on a managed switch between the devices — mirror the
   two players' ports to ours. Non-invasive, cleanest.
2. **Network TAP** inline on a player's link. Passive, reliable.
3. **Real Ethernet hub** (not a switch) inserted in the path — floods all ports.
4. **ARP-spoof MITM** from our host (bettercap / arpspoof + IP forwarding), the
   technique used in `dysentery/.../stagehand.adoc`. No extra hardware, but
   invasive; only appropriate on a test rig, never a live set.

Confirmed out-of-band (direct probes to player 1, not in this pcap): the player
answers portmap DUMP (nfs v2 udp/2049, mountd v1 udp/48353) and the dbserver
port lookup on TCP 12523. NFS `MNT /C/` and `/B/` are refused with `EACCES` to
our virtual CDJ under every presentation tried (AUTH_NULL and AUTH_UNIX uid 0,
privileged source port, announced as an online peer, and posing as a
`CDJ-3000X` model `0xe4`). Real players do not refuse each other, so the
CDJ-3000X firmware gates NFS access by something beyond peer presence.
