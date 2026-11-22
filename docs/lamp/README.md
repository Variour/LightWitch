# Lamps: physical brightness and spatial layout

A *lamp* is a device whose LEDs sit on modules (for now the LED triangles
with 32 mm grid edge of the Smart Laterne project) with different LED types, counts and
positions. Unlike a strip or matrix, every pixel can be different: one pixel
is a warm/cool/amber white chip, the next drives three amber LED pairs at
three different places on the board. This document defines:

1. the **brightness model** LightWitch uses for lamps (physical units, uniform
   density across mixed modules, optional peaks),
2. the **lamp file** (`lightwitch-lamp`, version 1): which modules a lamp is
   made of, where they are, how they are chained, what every channel emits,
3. the **import path** from a configurator into the device,
4. how **spatial patterns** (fire rising upward) use the layout.

The module library with the current triangle types is
[`modules.json`](modules.json).

## 1. Brightness model

### Unit

LightWitch describes every LED channel by its **luminous flux at 100 % duty in
lumen (lm)**. Lumen is a property of the light source alone. Lux (lx) depends
on distance and geometry and luminance (cd/m²) on the shade, so neither works
as a per-channel value. Lux stays useful for describing scenes (section 1.5).

Lamp brightness is controlled as **area brightness** `M` in **lm/dm²**: luminous
flux per square decimetre of module area (grid area, a 32 mm triangle is
4.434 cm² = 0.04434 dm²). Equal area brightness means a shade at the same
distance looks equally bright everywhere, no matter whether the triangle
behind it carries one RGB chip or three.

### The fixed scale

The 8-bit group brightness `b` (0-255, the value groups already sync over the
mesh) maps onto a fixed logarithmic scale on every lamp:

```
b = 0          off
b = 1 .. 255   M(b) = 0.5 lm/dm² × 4000^((b - 1) / 254)     (0.5 .. 2000 lm/dm²)
```

Each step is about 3.3 % brighter than the one before, which looks even to the
eye and gives smooth dimming. Because the scale is fixed, the same `b` means
the same physical brightness on every lamp in the mesh. No mesh message
changes. Lights without a lamp file keep the old linear meaning of `b`.

| b | lm/dm² | per 32 mm triangle |
|---|---|---|
| 1 | 0.5 | 0.02 lm |
| 32 | 1.4 | 0.06 lm |
| 64 | 3.9 | 0.17 lm |
| 100 | 12.7 | 0.56 lm |
| 128 | 32 | 1.4 lm |
| 160 | 90 | 4.0 lm |
| 192 | 256 | 11 lm |
| 224 | 727 | 32 lm |
| 255 | 2000 | 89 lm |

The 8-bit PWM of the LED ICs limits the low end: the smallest step of one
channel is 1/255 of its maximum. Per module that is between about 0.03 lm/dm²
(blue of RGB Matrix 1) and about 2 lm/dm² (warm white on Weiß Ambiente).
Below its step a channel stays dark; on Weiß Ambiente this means warm white
starts at about b = 43. Temporal dithering would lower this and is not
implemented yet.

### Which channels render a colour

Patterns still render colours as RGB (0-255). The channels of a module fall
into **families**:

- `rgb`: emitters with r/g/b channels (WS2812C),
- `white`: emitters with warm white/cool white channels, the SKC6812WWA chips (their amber channel belongs to them),
- `amber`: emitters with only amber channels, the amber pairs on WS2811F.

On each module exactly one family renders a given colour, the others stay
dark. With `ĉ` the colour divided by its largest component:

1. `amber`, if the module has it and the colour is a deep warm tone (`ĉg ≤ 0.45`, `ĉb ≤ 0.15`): glow on Kerze.
2. `rgb`, if the module has it and the white chips cannot mix the colour: it is not ordered `ĉr ≥ ĉg ≥ ĉb`, or it is fully saturated (`min(ĉ) ≤ 0.05`).
3. `white`, if the module has it.
4. otherwise `rgb`, then `amber`.

So Kerze shows red and deep orange with its amber pairs and candle and warm
white tones with its WWA chip. RGB Ambiente shows warm and cool whites with
its WWA chip and every other colour with its RGB chips. Modules without RGB
approximate any colour by the nearest white/amber mix (blue becomes cool
white, green warm white).

Within a family, the colour becomes channel weights:

| Family | Weights |
|---|---|
| rgb | r = ĉr, g = ĉg, b = ĉb |
| white | amber = max(0, ĉr - ĉg), warm white = max(0, ĉg - ĉb), cool white = ĉb |
| amber | amber = ĉr |

### Emitters and their area

An **emitter** is the set of channels of one pixel at one position. A WWA or
RGB chip is one emitter with three channels; a WS2811F driving three amber
pairs is three emitters with one channel each.

A module's grid area is shared equally by its pixels of the same family, and a
pixel's share equally by its emitters. `A` below is that area in dm²:

- Weiß Ambiente: 3 WWA emitters, each 1/3 of the module area.
- Kerze: the WWA emitter gets the full area (white family). The amber family has 2 ICs with 3 emitters each, so each amber pair gets 1/6.
- RGB Ambiente: the WWA emitter gets the full area, each RGB emitter 1/3.

### Channel duty

The intensity of a colour is `s = max(r, g, b) / 255`. The emitter that
renders the colour emits `s × M × A` lumen, split over its channels by the
weights. Its duty factor is

```
k = s × M × A / Σ_j (w_j × lm_j)
duty_i = w_i × k
```

If the largest `duty_i` exceeds 1, `k` is reduced so it becomes exactly 1
(the emitter clips as a whole and keeps its hue). The 8-bit value is
`round(duty_i × 255)`. Duties are linear PWM, there is no gamma: the
logarithmic scale already takes care of perception.

### Uniform mode and peaks

Per device, `lampMode` selects how `M(b)` is applied:

| lampMode | Name | Behaviour |
|---|---|---|
| 0 | Raw | Old behaviour: pattern colours go to the wire unchanged (r/g/b channels take their component, white and amber channels the largest component), `b` scales linearly. |
| 1 | Uniform (default) | `M` is capped at the lamp's uniform ceiling for the current colour. The whole lamp shows the same area brightness. |
| 2 | Uniform with peaks | No cap. Every emitter follows `M` until it clips at its own maximum. Below the ceiling this looks the same as Uniform, above it the stronger modules keep rising. |

The **uniform ceiling** for a colour is the highest `M` that every emitter
which renders this colour can still reach. LightWitch computes it from the
lamp: for each family `f` and channel kind `k` it stores `D[f][k]`, the
smallest `lm_k / A` of all emitters of that family. For a colour, every module
type present picks its family `f` (rules above), and that family can reach

```
ceiling_f = min over kinds i with w_i > 0 of  Σ_j (w_j × D[f][j]) / w_i
```

The lamp's ceiling is the smallest `ceiling_f` over the families picked. This
is exact as long as all emitters of a family use the same chip, which holds
for every current module. Only outputs mapped to a light count.

Examples (estimated values):

| Lamp | Colour | Ceiling |
|---|---|---|
| Kerze + RGB Matrix 1 | candle (255, 147, 41) | 36 lm/dm² (the single RGB chip limits, the Kerze WWA is held to it) |
| Kerze + RGB Matrix 1 | red (255, 40, 0) | 20 lm/dm² (RGB red limits, the amber pairs are held to it) |
| Weiß + Kerze + RGB Ambiente | warm white | 447 lm/dm² (single WWA chip of Kerze and RGB Ambiente) |

Maximum area brightness per triangle type and family (all channels of the
family at 100 %, estimated catalog values; the UI shows this per lamp):

| Triangle | Family | lm at full | lm/dm² |
|---|---|---|---|
| Weiß Ambiente | white (3 WWA) | 59.4 | 1340 |
| Kerze | white (1 WWA) | 19.8 | 447 |
| Kerze | amber (6 pairs) | 1.5 | 34 |
| RGB Ambiente | white (1 WWA) | 19.8 | 447 |
| RGB Ambiente | rgb (3 RGB) | 7.7 | 175 |
| RGB Matrix | rgb (3 RGB) | 7.7 | 175 |
| RGB Matrix 1 | rgb (1 RGB) | 2.6 | 58 |

### Describing brightness

Reference values, as estimates for talking about scenes:

| Light source | Flux |
|---|---|
| bought LED tealight | 0.5-2 lm |
| real candle or tealight flame | about 12 lm |
| incandescent bulb 25 W | about 250 lm |
| incandescent bulb 40 W | about 470 lm |
| incandescent bulb 60 W | about 800 lm |

| Situation | Illuminance |
|---|---|
| full moon | 0.2 lx |
| finding your way on a path | 1-5 lx |
| dinner for two (project target) | about 40 lx |
| cosy living room table | 50-100 lx |
| board game, reading | about 300 lx |

For a lamp that radiates evenly in all directions, illuminance at distance
`d` is about `E = Φ / (4π d²)`: 100 lm give about 32 lx at 0.5 m and 8 lx at 1 m.
A lamp of 15 triangles in one family at `b = 128` (32 lm/dm²) emits
`15 × 0.0443 dm² × 32 lm/dm² ≈ 21 lm`.

## 2. Lamp file (`lightwitch-lamp`, version 1)

JSON, UTF-8. Coordinates in millimetres.

```json
{
  "format": "lightwitch-lamp",
  "version": 1,
  "name": "Teelicht",
  "types": {
    "kerze": { "name": "Kerze", "area": 443.4,
               "outline": [[-13.01, -4.06], [-10.02, -9.24], [10.02, -9.24], [13.01, -4.06], [2.99, 13.3], [-2.99, 13.3]],
               "pixels": [
                 { "ch": [ { "k": "ww", "lm": 7.2, "mA": 15, "at": [0, 0] },
                           { "k": "cw", "lm": 7.9, "mA": 15, "at": [0, 0] },
                           { "k": "a",  "lm": 4.7, "mA": 15, "at": [0, 0] } ] },
                 { "ch": [ { "k": "a", "lm": 0.25, "mA": 16.5, "at": [[-8.0, -1.54], [-10.66, -6.16]] },
                           { "k": "a", "lm": 0.25, "mA": 16.5, "at": [[5.33, -6.16], [10.66, -6.16]] },
                           { "k": "a", "lm": 0.25, "mA": 16.5, "at": [[2.67, 7.69], [0.0, 12.31]] } ] }
               ] }
  },
  "outputs": [
    { "id": "A", "light": 0,
      "modules": [ { "id": "m1", "t": "kerze", "p": [0, -5.33, 8.71], "n": [0, -0.943, 0.333], "u": [0, 0.333, 0.943] } ] }
  ]
}
```

| Field | Meaning |
|---|---|
| `format`, `version` | Always `"lightwitch-lamp"` and `1`. A later incompatible change bumps `version`. |
| `name` | Display name, at most 31 characters are kept. |
| `types` | Module types used by this lamp, keyed by type id. Copy them from [`modules.json`](modules.json). The file carries its own copy so the device never needs a library. |
| `types.*.area` | Grid area of the module in mm², the base of the area brightness. |
| `types.*.outline` | Optional, module outline in local coordinates, for previews. |
| `types.*.pixels` | Pixels (one LED IC each: WS2811F, SKC6812WWA, WS2812C) in chain order inside the module. |
| `pixels[].ch` | Exactly three channels in **wire order** (WS2812C: g, r, b). |
| `ch[].k` | Channel kind: `r`, `g`, `b`, `ww` (warm white), `cw` (cool white), `a` (amber) or `""` (not connected). |
| `ch[].lm` | Luminous flux at 100 % in lm. |
| `ch[].mA` | Optional, channel current at 100 % in mA. |
| `ch[].at` | Position in local coordinates: one point `[x, y]` or a list of points for several LEDs on one channel (amber pairs). LightWitch uses their centre. |
| `outputs` | One entry per data output of the controller (L: A-D, S: A/B). |
| `outputs[].id` | Output name as printed on the board. |
| `outputs[].light` | LightWitch light slot (0-3) this output drives. Set by the importing UI, a configurator leaves it out. |
| `outputs[].modules` | Modules in chain order from the controller outwards. |
| `modules[].id` | Optional instance id from the configurator (for the parts list, previews). |
| `modules[].t` | Type id from `types`. |
| `modules[].p` | Position of the module's local origin in lamp coordinates. |
| `modules[].n` | Normal: the direction the LED side faces. Normalised on import. |
| `modules[].u` | Direction of the local +y axis in lamp coordinates. Made orthogonal to `n` and normalised on import (Gram-Schmidt). |

**Module coordinates:** origin in the centroid of the grid triangle, `+y`
towards the tip opposite the data input edge (the IN edge is the base at
`y = -9.24`), `+x` to the right when looking at the LED side.

**Lamp coordinates:** `+z` up. For lamps that stand, the origin is the centre
of the lamp's bottom; for flat pieces such as a wall matrix any origin works.
Patterns use heights relative to the lowest and highest emitter.
A point `(x, y)` of a module lies at `p + x·r + y·u` with `r = u × n`.

A triangle "point up" has `u` pointing upwards, "flat side up" has `u`
pointing downwards. The firmware does not care, it only uses the resulting
positions.

### Limits

| | esp32c3 (board S) | others |
|---|---|---|
| file size | 48 KB | 160 KB |
| outputs | 4 | 4 |
| pixels per lamp | 1024 | 1024 |

### Validation on import

The device rejects a file when `format`/`version` do not match, the file or
pixel count exceeds the limits, a module references an unknown type, a type
has no positive `area`, a module lacks `p`/`n`/`u` or `u` is parallel to `n`,
a pixel does not have exactly three channels, an unknown channel kind appears,
a channel has no `at`, a `light` index does not name an existing light, or
two outputs name the same light.

## 3. Import

| Endpoint | |
|---|---|
| `GET /api/lamp` | `{ "loaded": false }` or `{ "loaded": true, "name", "mode", "pixels", "outputs": [{ "id", "light", "modules", "pixels" }], "families": { "rgb": { "ceiling", "maxLm", "area" }, ... } }`. `ceiling` in lm/dm², `maxLm` = lumen of the family at its ceiling, `area` in dm². |
| `GET /api/lamp/file` | The stored lamp file, unchanged. |
| `POST /api/lamp` | Body: lamp file with `light` set on each output that should be used. The device validates it, stores it as `/lamp.json`, sets every mapped light to a WS2812 string of the output's pixel count with wire order RGB, and restarts. |
| `POST /api/lamp/mode` | `{ "mode": 0 \| 1 \| 2 }`, applied at once. |
| `POST /api/lamp/delete` | Removes the lamp file and restarts. The lights keep their pixel counts. |

From a configurator on an https page a browser cannot post to a device on
plain http. The configurator therefore opens the device UI with the lamp in
the URL fragment:

```
http://<device>.local/#lamp=<base64url(deflate-raw(lamp file as UTF-8))>
```

The device UI decodes it, shows a preview with the mapping of outputs to
lights and posts it after confirmation. Uploading the file in the UI is the
fallback.

## 4. Spatial patterns

The pattern **Fire** (`PatternId` 5) uses the lamp layout: heat comes from
3D value noise that moves upward in lamp coordinates, so flames rise, nearby
emitters on the same side flicker together and emitters on opposite sides
flicker independently. Brightness falls off with height between the lowest
and the highest emitter. Colours run from dark red through orange and yellow
to warm white, so by the family rules the amber pairs carry the glow and the
WWA chips the bright flame. Amber pairs on one IC sit at different places and
each samples its own position. On lights
without a lamp, Fire treats the LED index as height.

Other patterns render as before. On a lamp, `setColor`/`setPixel` go through
the brightness model above.
