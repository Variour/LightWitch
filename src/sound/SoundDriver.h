#pragma once
#include <stdint.h>

// Abstract sound output interface. Swap this implementation when the codec
// chip changes (see LedDriver.h for the equivalent on the light side).
// Hardware bring-up only for now — there is no notion yet of "what plays";
// playTestMelody() exists purely to let the user verify pin wiring from the
// web UI, the same way the LED side's showColorOrderTest() does.
class SoundDriver {
   public:
    virtual ~SoundDriver() = default;
    virtual void begin() = 0;
    virtual void playTestMelody() = 0;
};
