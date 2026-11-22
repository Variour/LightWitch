# Lamp configurator

A static web page to design a lamp from 32 mm LED triangles in 3D, plan its data chains,
get the parts list and hand the result to LightWitch as a lamp file.

```sh
cd tools/lamp-configurator
npm start        # http://localhost:8090/
npm test         # core tests in Node, no browser needed
```

No build step and no dependencies: HTML, ES modules and three.js 0.160 from jsDelivr.
Any static server works; `bin/serve.js` is a small one.

## Address and deep links

`.github/workflows/pages-konfigurator.yml` publishes `public/` to GitHub Pages under
`/konfigurator/` on every push to `main` that touches it (one-time setup: Settings → Pages →
Source: GitHub Actions). Address: `https://variour.github.io/LightWitch/konfigurator/`.
When a custom domain is set for the repository's Pages later, GitHub redirects the
`github.io` address to it and keeps path and `#`, so printed QR codes keep working.

A plain `#name` opens a catalog lamp with its light on: `#teelicht`, `#oellampe-glas`,
`#oellampe-griff`, `#oellampe-sturm` (same LED body as `#oellampe`), `#allrounder`,
`#magische-kugel`, `#wurfball`, `#led-matrix`. `#p=<packed project>` opens a shared project.

The "Licht" button shows the lamp lit (black boards, glowing LEDs). `window.__cfg.snapshot()`
renders a transparent PNG for the catalog.

## Steps in the page

1. **Start**: a catalog lamp (Teelicht, Öllampe, Allrounder, Magische Kugel, Wurfball,
   RGB-Matrix), a generator (zigzag ring bands with caps, flat area, platonic body with an
   optional gap to a core) or a project file / `#p=` link.
2. **Form**: point at a free (yellow) edge to get a ghost triangle, set the fold angle with the
   mouse wheel (Shift: 1°) or the slider, click to place. If the ghost's free corner comes within
   4 mm of an existing corner at an angle within 12° of the chosen one, it snaps there (green)
   and closes the gap. Edges that end up on top of each other become seams.
3. **Bestückung**: paint module types; double click paints the whole band (same height and tilt).
4. **Verkabelung**: pick control board S (outputs A, B) or L (A to D) and the battery, paint
   outputs. Chains are planned automatically (see below). "Kette festschreiben" stores the order
   in the project so a soldered lamp keeps its chain.
5. **Teile**: parts list with stock (kept in the browser, exportable), CSV and print.
6. **LightWitch**: enter the lamp's address and click. The page opens
   `http://<address>/#lamp=<deflate-raw + base64url>`; the LightWitch UI shows a preview and
   imports it into `POST /api/lamp`. Fallback: download the lamp file and upload it there.
   A https page cannot `fetch` a http device in the LAN, hence the link.

## Chains and bridges

Each board has one IN edge (the datasheet base) and two OUT edges on the same net; only one
OUT is continued. A chain is therefore a path through neighbouring triangles that visits every
triangle of the output once, and each board is rotated so its IN edge faces its predecessor.
The planner tries a depth-first search that prefers the neighbour with the fewest onward
options (bounded steps, several start triangles near the control board). If that finds no
path, it walks greedily and continues over 3-wire jumpers, which appear in the parts list.
Loose triangles (the balls) are always chained with jumpers.

Seams get one of three bridges:

| Seam | Pads bridged | Part |
|---|---|---|
| chain step | GND, 5V, DATA, 5V, GND | 5-pin header |
| same output, not a chain step | GND, 5V, 5V, GND | 5-pin header, middle pin pulled |
| border between outputs | GND, GND | 2 single pins on the outer pads |

The first board of each output gets a 3-wire cable from the control board, soldered to the
holes behind its IN edge.

## Files

| File | Content |
|---|---|
| `public/core/geometry.js` | triangles, seams, docking, snapping, generators (no DOM, no three.js) |
| `public/core/catalog.js` | editor colours and prices, control boards, packs, catalog lamps |
| `public/core/wiring.js` | chain planning, board rotation, bridge classes |
| `public/core/bom.js` | parts list, stock, current, lumen, runtime |
| `public/core/export.js` | lamp file, project file, links |
| `public/data/lamp-modules.json` | module types in the lamp format (working copy, see below) |
| `public/app/` | 3D view and panels |

## Data formats

**Lamp file** (`lightwitch-lamp` v1): `types` (module types used, pixels and channels with
lumen at 100 %) and `outputs` with the modules in chain order. Each module has `p` (grid
centroid), `n` (LED side normal) and `u` (from the IN edge towards the apex); module-local
`(x, y)` maps to `p + x*r + y*u` with `r = u × n`. World frame: mm, z up, origin at the centre
of the lamp's foot. The format and the module library are defined with the LightWitch
brightness work (`docs/lamp-format.md`, `docs/lamp-modules.json`); `public/data/lamp-modules.json`
is a copy with the same fields until that lands. Lumen and current values are estimates.

**Project file** (`lightwitch-lamp-project` v1): name, control board, pack and per triangle
`id`, `v` (three corners in mm, counter-clockwise seen from the LED side), `type`, `out` and,
once locked, `pos` (place in the chain). Seams, rotation, pixels and parts are derived.
