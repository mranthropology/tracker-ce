// Braids (Mutable Instruments macro oscillator) as a Tracker instrument engine
// on the original Tracker (MK66, FEATURE_BRAIDS). See README.md.
#pragma once

#include <stdint.h>

#ifdef FEATURE_BRAIDS

namespace braidsEngine
{
// Braids voices that can sound at once (CPU budget on the 180 MHz M4).
constexpr int kMaxVoices = 4;
constexpr int kNumModels = 48;

// "BR-<model>" (or "BR:<model>") -> model index 0..47, else -1. `name` is the
// instrument's 32-byte name field; a missing or unknown model gives 0.
int parseModel(const char* name);

// Take a free voice for `owner` (any stable pointer, e.g. the player), or -1
// if all kMaxVoices are in use. A voice already held by `owner` is reused.
int acquire(const void* owner);
void release(int voice);

// Note-on: (re)initialise the oscillator for `model` if needed and strike it.
void noteOn(int voice, int model);

// One 128-sample block. pitch: MIDI note * 128 (C5 = 60 * 128 on the Tracker's
// note scale); timbre, color: 0..32767. Output is 6 dB below Braids' level.
// With `shed` false (offline export) the load guard is ignored.
void render(int voice, int model, int pitch, int timbre, int color, int16_t* out, bool shed = true);

// Load guard, called once per live audio update with its duration. Above 90%
// of the block period the engine sheds load a step at a time: first the
// 96 kHz models render directly (cheaper, longer decays), then the most
// expensive voices are silenced one by one. Below 75% for about a second a
// step is restored. Keeps the UI (main loop) from starving.
void audioUpdateDone(int updateUs, int periodUs);

// Helpers for the player: playback-rate ratio -> Braids pitch, and the
// granular grain length (samples) -> Color on a log scale.
int pitchFromRate(float rateOverC5);
int16_t colorFromGrainLength(uint16_t samples);
} // namespace braidsEngine

#endif // FEATURE_BRAIDS
