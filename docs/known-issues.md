# Known Issues

## WiFi channel usage: non-standard channels can break peer discovery

The mesh channel logic only auto-searches the previously stored channel plus channels `1`, `6`, and `11`. In addition, fallback AP mode is started on channel `1`.

This means setups that rely on non-standard 2.4 GHz channels such as `2–5`, `7–10`, `12`, or `13` can behave unexpectedly:

- devices may fail to discover peers after boot if no channel has been stored yet
- devices can split across channels after WiFi changes or resets
- manual **Search channel** may still not find peers unless one device is already locked to the target channel and can teach it to others

In practice, the system is most reliable when the WiFi network uses channel `1`, `6`, or `11`.

## Mesh channel islands: simultaneous multi-device boot without WiFi can split the mesh

`ChannelManager` elects an ESP-NOW channel by having WiFi-less devices search `[stored, 1, 6, 11]` (deduplicated) and lock onto whichever channel they first hear a peer heartbeat on. This works reliably when at least one device is actually WiFi-connected — it locks straight to the router's real channel, and everyone else eventually finds it during their own search. There is no such anchor in a common field scenario: several battery-powered devices with no WiFi configured, powered on together away from any router. Each device searches independently with its own randomized dwell time and no coordination between devices, so two (or more) subsets can each find each other and lock before ever overlapping with the other subset — producing multiple separate, internally-connected mesh islands that never learn about each other.

This is distinct from the non-standard-channel limitation above: it can happen purely within channels `1`/`6`/`11`, with no WiFi involved at all. It is also distinct from ordinary peer loss — once a device is `Locked`, it never re-checks whether it's still actually hearing anyone; only an active search (at boot, or a manual **Search devices** click in the web UI) re-evaluates the channel.

Mitigation in place (`ChannelManager`, #321):
- Per-channel dwell nudged up slightly, 6–9 s → 7–10 s (`_randomDwell`) — still comfortably covers the 5 s heartbeat period.
- Search now runs **two full passes** over `[stored, 1, 6, 11]` (`SEARCH_ROUNDS`) before giving up, not one — a second pass gives devices on a different boot phase, or a different stored-channel search order, another independent chance to land on the same channel at the same time.
- If both passes end with no peer heard, every device falls back to one **common channel (`COMMON_FALLBACK_CHANNEL`, currently `1`)** rather than each device's own stored channel. Devices with different WiFi history that never overlapped during search still converge on a shared channel instead of a silent, permanent split.

This reduces but does not eliminate the odds of island formation during search — it's a boot-time probability improvement, not a guarantee, and none of it costs anything once a device is `Locked`. The escape hatch remains the **Search devices** button. See #321 for the fuller design discussion, including automatic-detection approaches (periodic re-scan while locked, gossip/anti-entropy peer-count checks) that were considered and deferred as disproportionate to a battery-powered device's power budget.

## OTA filesystem update: scene backup limited by available heap

When a firmware update includes a `littlefs.bin` asset, custom scenes are read into heap memory before the filesystem is flashed and written back afterwards. If the total size of all scene files exceeds available heap (typically ~150–200 KB free on ESP32), the backup will silently drop scenes that could not be allocated. In practice, current scenes are a few KB in total and this limit is not a concern, but very large or numerous scenes could be lost.

## Canvas zoom: leftmost gradient column narrower than other columns

When gradient buttons are visible in the scene editor, the leftmost column contains the `↕` (vertical gradient fill) button. This button uses `rowspan` to span all data rows, which causes browsers to exclude it from column-width calculation in auto table layout. As a result, the column can become narrower than all other columns at small zoom levels.

Attempted fixes:
- Adding `width`/`min-width` CSS on the cell — ignored for rowspan cells in auto layout
- Adding explicit-width non-rowspan cells in the same column in header rows — still ignored
- `<colgroup>` with `width: var(--cell-size)` on `<col>` — CSS custom properties do not cascade to `<col>` elements
- `table-layout: fixed` with `<colgroup>` — column widths worked but zoom stopped affecting cell widths for the same reason (CSS vars don't cascade to `<col>`)
- `table-layout: fixed` with inline pixel widths on `<col>`, updated via JS — JS updates to `<col>` styles do not trigger table reflow reliably

Root cause: the combination of `rowspan` on the `↕` cell and browser table layout behaviour makes it impossible to reliably enforce a minimum column width for that column without restructuring the table (e.g. removing `rowspan` and duplicating the button per row).
