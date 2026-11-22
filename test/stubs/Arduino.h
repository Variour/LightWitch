#pragma once
// Host stand-in for the Arduino core: only what the headers under test use.
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

inline unsigned long millis() { return 0; }
