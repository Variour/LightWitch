## Why no external TCA9555 library

`Tca9555Expander.cpp` is an original implementation written against the
publicly documented TCA9555 register map (NXP/TI, an industry-standard I2C
GPIO expander — register addresses and their purpose are factual/functional
information, not copyrightable expression), not a vendored copy of an
existing Arduino library. It uses the Arduino `Wire` library, already a
transitive dependency of this project's Arduino core — no new entry in
`platformio.ini`'s `lib_deps`, so no third-party license to track.

This directory holds generic hardware-adjacent building blocks (currently
just the expander) that aren't specific to any one feature (e.g. `src/sound/`)
so they can be reused elsewhere without pulling in unrelated code.
