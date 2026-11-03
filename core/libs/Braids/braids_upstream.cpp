// Compiles the unmodified Mutable Instruments Braids / stmlib sources in
// upstream/ (MIT, see LICENSE-BRAIDS.txt) as one translation unit.
#ifdef FEATURE_BRAIDS
#ifndef TEST
#define TEST   // Braids' host build: no STM32 peripherals
#endif
#include "braids/macro_oscillator.cc"
#include "braids/analog_oscillator.cc"
// digital_oscillator.cc repeats three file-local constants from
// analog_oscillator.cc; rename them for this translation unit only.
#define kHighestNote kHighestNote_digital
#define kPitchTableStart kPitchTableStart_digital
#define kOctave kOctave_digital
#include "braids/digital_oscillator.cc"
#undef kHighestNote
#undef kPitchTableStart
#undef kOctave
#include "braids/resources.cc"
#include "stmlib/utils/random.cc"
#endif  // FEATURE_BRAIDS
