# Known Issues

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
