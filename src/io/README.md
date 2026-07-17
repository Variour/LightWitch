## TCA9555: external library, MIT-licensed

`Tca9555Expander` wraps [robtillaart/TCA9555](https://github.com/RobTillaart/TCA9555)
(MIT license, declared in `platformio.ini`'s `lib_deps`) rather than
reimplementing the register protocol — this matches how the LED drivers
(`Ws2801Driver`/`Ws2812bDriver`) wrap `Adafruit_WS2801`/`Adafruit_NeoPixel`.
An original implementation was considered (see `src/sound/README.md` for why
that path was taken for the ES8311 codec instead), but for the TCA9555
specifically a well-tested, permissively-licensed, PlatformIO-registry
library already existed with no integration friction, so there was no
reason not to use it.

Note: `Tca9555Expander` deliberately never calls the library's own `begin()`
— it reconfigures all 16 pins in one call, which would clobber whatever else
is wired to the other bits on boards that multiplex several unrelated
peripherals across the same expander (e.g. LCD reset, buttons, camera power,
alongside a speaker-amp enable line). `pinMode1()`/`write1()` do a proper
per-pin read-modify-write instead — see `beginOutput()`/`write()`.

This directory holds generic hardware-adjacent building blocks (currently
just the expander) that aren't specific to any one feature (e.g. `src/sound/`)
so they can be reused elsewhere without pulling in unrelated code.
