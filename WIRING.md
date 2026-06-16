# Wiring

The LED type is configurable per device in the web UI under **Network → LED Type**.

> **Board-specific pins:** ESP32-WROOM-32 uses GPIO 25 (data) / GPIO 26 (clock). ESP32-C3 uses GPIO 20 (data) / GPIO 21 (clock). To change pins, update `LED_DATA_PIN` / `LED_CLOCK_PIN` in the `build_flags` section of `platformio.ini`.

---

## ESP32-WROOM-32

### WS2812B (default)

Single-wire NeoPixel-style LEDs with a built-in constant-current driver.
No per-channel current resistors needed.

#### Components

| Component | Notes |
|-----------|-------|
| ESP32-WROOM-32 | |
| WS2812B LEDs | Strip or array |
| 100 µF electrolytic capacitor | Across VCC / GND, near the LED |
| 300–500 Ω resistor | On the data line |

#### Connections

```
ESP32-WROOM-32          WS2812B
──────────────          ───────
5V  (or VIN) ──────────── VCC
GND          ──────────── GND
GPIO 25 ──── 300–500 Ω ── DIN
                           DOUT  (leave unconnected for a single LED)
```

> **Power note:** WS2812B requires 5 V on VCC. The ESP32's 3.3 V data signal is usually
> sufficient, but add a 74AHCT125 level shifter between GPIO 25 and DIN if you see erratic
> colours.

#### Decoupling capacitor

Place as close as possible to the LED's VCC / GND pins to absorb current spikes.

```
5V rail ──┬──── WS2812B VCC
          ═ 100 µF
          ─
GND ──────┴──── WS2812B GND
```

---

### WS2801

Two-wire SPI LEDs (separate data and clock lines). Slower protocol but more tolerant
of long cable runs and 3.3 V logic levels.

#### Components

| Component | Notes |
|-----------|-------|
| ESP32-WROOM-32 | |
| WS2801 LEDs | Strip or module |
| 100 µF electrolytic capacitor | Across VCC / GND, near the LED |

No series resistors needed — the WS2801 samples on the clock edge, so line ringing is not an issue.

#### Connections

```
ESP32-WROOM-32          WS2801
──────────────          ──────
5V  (or VIN) ──────────── VCC  (5 V)
GND          ──────────── GND
GPIO 25      ──────────── DAT  (data / SDI / DI)
GPIO 26      ──────────── CLK  (clock / CKI / CI)
                           DO   (leave unconnected for a single LED)
                           CO   (leave unconnected for a single LED)
```

> **Label note:** Different breakout boards label the lines differently.
> DAT / SDI / DI / DATA all mean the data input; CLK / CKI / CI / CLOCK all mean clock.

> **Voltage note:** The WS2801 power supply must be 5 V, but its data and clock inputs
> are 3.3 V-compatible, so no level shifter is required.

#### Decoupling capacitor

```
5V rail ──┬──── WS2801 VCC
          ═ 100 µF
          ─
GND ──────┴──── WS2801 GND
```

#### Chaining multiple LEDs

WS2801 strips daisy-chain via DO → DI and CO → CI between modules.
The firmware drives `ACTIVE_LEDS` at the head of the strip and blanks the rest on startup.

---

## ESP32-C3

The ESP32-C3 is a RISC-V single-core chip with built-in USB Serial/JTAG (GPIO 18/19).
GPIO 20 and 21 are used for LED data and clock and are free from USB conflicts.

### WS2812B (default)

#### Components

| Component | Notes |
|-----------|-------|
| ESP32-C3 | e.g. ESP32-C3-DevKitM-1 |
| WS2812B LEDs | Strip or array |
| 100 µF electrolytic capacitor | Across VCC / GND, near the LED |
| 300–500 Ω resistor | On the data line |

#### Connections

```
ESP32-C3                WS2812B
────────                ───────
5V  (or VIN) ──────────── VCC
GND          ──────────── GND
GPIO 20 ──── 300–500 Ω ── DIN
                           DOUT  (leave unconnected for a single LED)
```

> **Power note:** WS2812B requires 5 V on VCC. The ESP32-C3's 3.3 V data signal is usually
> sufficient, but add a 74AHCT125 level shifter between GPIO 20 and DIN if you see erratic
> colours.

#### Decoupling capacitor

```
5V rail ──┬──── WS2812B VCC
          ═ 100 µF
          ─
GND ──────┴──── WS2812B GND
```

---

### WS2801

#### Components

| Component | Notes |
|-----------|-------|
| ESP32-C3 | e.g. ESP32-C3-DevKitM-1 |
| WS2801 LEDs | Strip or module |
| 100 µF electrolytic capacitor | Across VCC / GND, near the LED |

No series resistors needed — the WS2801 samples on the clock edge, so line ringing is not an issue.

#### Connections

```
ESP32-C3                WS2801
────────                ──────
5V  (or VIN) ──────────── VCC  (5 V)
GND          ──────────── GND
GPIO 20      ──────────── DAT  (data / SDI / DI)
GPIO 21      ──────────── CLK  (clock / CKI / CI)
                           DO   (leave unconnected for a single LED)
                           CO   (leave unconnected for a single LED)
```

> **Voltage note:** The WS2801 power supply must be 5 V, but its data and clock inputs
> are 3.3 V-compatible, so no level shifter is required.

#### Decoupling capacitor

```
5V rail ──┬──── WS2801 VCC
          ═ 100 µF
          ─
GND ──────┴──── WS2801 GND
```

#### Chaining multiple LEDs

WS2801 strips daisy-chain via DO → DI and CO → CI between modules.
The firmware drives `ACTIVE_LEDS` at the head of the strip and blanks the rest on startup.
