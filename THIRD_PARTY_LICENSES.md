# Third-Party Licenses

LightWitch itself is licensed under the GNU General Public License v3.0
(see [`LICENSE`](LICENSE)). The compiled firmware binaries published as
GitHub Releases statically link the following third-party libraries, which
remain under their own licenses.

## Bundled libraries (`platformio.ini` → `lib_deps`)

| Library | Version | License | Copyright |
|---|---|---|---|
| [esp32async/ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | 3.11.2 | LGPL-3.0-or-later | (c) 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles |
| [esp32async/AsyncTCP](https://github.com/ESP32Async/AsyncTCP) | 3.4.10 | LGPL-3.0-or-later | (c) 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles |
| [adafruit/Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) | 1.15.5 | LGPL-3.0-or-later | (c) Adafruit Industries |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.4.3 | MIT | (c) 2014-2026 Benoit Blanchon |
| [adafruit/Adafruit WS2801 Library](https://github.com/adafruit/Adafruit-WS2801-Library) | 1.1.3 | BSD † | (c) Adafruit Industries (Limor Fried/Ladyada) |
| [knolleary/PubSubClient](https://github.com/knolleary/pubsubclient) | 2.8.0 | MIT | (c) 2008-2020 Nicholas O'Leary |

† The library's `README.md` states "BSD license, all text above must be
included in any redistribution" (quoted in full below); the Doxygen
header in `Adafruit_WS2801.cpp` states "MIT license" instead. Both texts
are reproduced below.

None of the above libraries have been modified from their upstream
releases; they are consumed unmodified via the PlatformIO registry at the
pinned versions shown.

## Toolchain / platform

`platform = espressif32 @ 7.0.1` (the Arduino-ESP32 core) is also
statically linked into the firmware and is itself licensed LGPL-2.1.

## License texts

### LGPL-3.0-or-later (ESPAsyncWebServer, AsyncTCP, Adafruit NeoPixel)

Full text: <https://www.gnu.org/licenses/lgpl-3.0.txt>

### LGPL-2.1 (Arduino-ESP32 core)

Full text: <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt>

### MIT (ArduinoJson)

```
The MIT License (MIT)
---------------------

Copyright © 2014-2026, Benoit BLANCHON

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### MIT (PubSubClient)

```
Copyright (c) 2008-2020 Nicholas O'Leary

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### BSD (Adafruit WS2801 Library)

Quoted verbatim from the library's `README.md`:

```
Adafruit invests time and resources providing this open source code,
please support Adafruit and open-source hardware by purchasing
products from Adafruit!

Written by Limor Fried/Ladyada for Adafruit Industries.
BSD license, all text above must be included in any redistribution
```
