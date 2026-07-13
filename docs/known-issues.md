# Known Issues

## WiFi channel usage: non-standard channels can break peer discovery

The mesh channel logic only auto-searches the previously stored channel plus channels `1`, `6`, and `11`. In addition, fallback AP mode is started on channel `1`.

This means setups that rely on non-standard 2.4 GHz channels such as `2–5`, `7–10`, `12`, or `13` can behave unexpectedly:

- devices may fail to discover peers after boot if no channel has been stored yet
- devices can split across channels after WiFi changes or resets
- manual **Search channel** may still not find peers unless one device is already locked to the target channel and can teach it to others

In practice, the system is most reliable when the WiFi network uses channel `1`, `6`, or `11`.

## Mesh channel islands: devices not currently WiFi-connected can split across channels

A device that isn't currently connected to WiFi searches independently for a channel with no coordination from other devices. Multiple such devices can each lock onto a different channel before ever hearing each other, forming separate mesh islands that don't know about each other. A manual **Search devices** re-search can merge those islands but isn't guaranteed to do so. See #321.

## Mesh channel split: devices connected to different WiFi networks never converge

`ChannelManager::begin()` locks straight to `WiFi.channel()` whenever WiFi is already connected at boot, with no search phase at all. If two or more devices are each configured for, and successfully connected to, a *different* WiFi network whose router happens to operate on a different channel, each device locks to its own router's channel and never re-evaluates:

- a continuously-connected device only reacts to a `WL_CONNECTED` *transition* in `tick()`, which already happened once at boot — it never re-checks after that
- a manual **Search devices** click doesn't help either: it flips the device to searching, but the very next `tick()` sees `WiFi.status()` is still connected and immediately re-locks to `WiFi.channel()` before ever looking elsewhere

Unlike the mesh channel islands case above, this is a permanent, deterministic split, not a probabilistic one — there is no automatic escape hatch. Two devices legitimately, correctly connected to their own configured routers simply never share ESP-NOW airtime. See #323.

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
