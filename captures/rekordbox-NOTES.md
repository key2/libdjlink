# rekordbox.pcap — analysis notes (2026-08-27)

Capture taken **on** a Windows rekordbox host (so rekordbox's own unicast is
visible, unlike a passive capture on a switched segment). ~150 k packets.

- Hosts: `169.254.185.63` rekordbox (device `0x11`), `169.254.7.185` CDJ-3000X (1),
  `169.254.7.162` CDJ-3000X (2), `169.254.116.4` DJM-A9 (33).
- The raw 18 MB pcap is **not** committed (size); these notes capture the findings.

## What the library already handles
Announcements/keepalive/claim (50000), beats/on-air + a master handoff `0x26/0x27`
(50001), CDJ status `0x0a` and mixer status `0x29`, media query/response `0x05/0x06`,
rekordbox-lighting hello `0x10`, and Touch-Audio timing `0x20` (122 k packets — the
DJM-A9 streaming to rekordbox). All surface correctly; unknown kinds surface as
`DJL_EV_UNKNOWN_PACKET`.

## What we do NOT handle (see ARCHITECTURE.md §1.11)

1. **No dbserver TCP at all.** With rekordbox as the source, players use NFS + UDP
   control instead of the `dbserver` TCP protocol.

2. **rekordbox is an NFS *server*; the CDJs mount it.** portmap(111)→mount(100005)/
   nfs(100003), `MNT "/"` (status 0), `LOOKUP` with **UTF-16LE** filenames
   (`rekordbox_customimage.jpeg`, `PIONEER`). Here the CDJs pull rekordbox's custom
   on-air image; a collection track load would also `READ` audio + PDB/ANLZ. We
   implement no NFS (client or server); serving a collection like rekordbox is the
   largest gap.

3. **Seven undocumented rekordbox↔CDJ/mixer UDP kinds on 50002:**
   `0x11` (rekordbox→CDJ, 296 B, carries the host computer name in UTF-16LE),
   `0x16` (rekordbox→CDJ keepalive), `0x30`/`0x31` (mixer↔rekordbox pair),
   `0x46`/`0x47` (CDJ↔rekordbox; `0x47` holds the `12 34 56 78` settings marker),
   `0x80` (CDJ→rekordbox notify). Currently surfaced as unknown; not decoded.
