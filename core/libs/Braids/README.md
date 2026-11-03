# Braids instrument engine (Original Tracker)

Mutable Instruments **Braids** (the macro oscillator) running as a real-time
instrument on the original Polyend Tracker (MK66). Built with `FEATURE_BRAIDS`,
which `core/makefile` defines for `PLATFORM=MK66` only.

## Status

Tested on an original Tracker (MK66) with CE 1.0.0. Experimental; not
official Polyend firmware.

## Using it

Name an instrument `BR-<model>` (for example `BR-FM`, `BR-PLUCKED`, `BR-12`;
case, spaces, `_` and `-` are ignored, a number selects the model by index)
and put it in **Granular** play mode. The sample is not played; Braids
synthesises the sound and the instrument's own chain (amp envelope, filter,
bit depth, sends, volume, pan) applies as usual.

| Braids | Tracker control |
|---|---|
| Pitch | note, tune, finetune, glide, pitch FX |
| Timbre | Granular Position (with its envelope, LFO and step FX) |
| Color | Granular Length (log scale; with its automation and the `z` step FX) |
| Model | the instrument name |

At most four Braids voices sound at once (CPU); a fifth note plays the
instrument's sample instead. Struck bell, struck drum, snare and particle
noise run at Braids' native 96 kHz and are resampled, because their decays
are per-sample constants.

## Sources

`upstream/` holds unmodified files from Emilie Gillet's Mutable Instruments
repositories, MIT licence (each file carries its notice):

- `pichenettes/eurorack` @ `f4cfa7697095a28d292bff758beb51deb49b5a86`:
  `braids/` macro, analog and digital oscillators, resources, excitation,
  parameter interpolation, settings, svf
- `pichenettes/stmlib` @ `d18def816c51d1da0c108236928b2bbd25c17481`:
  `stmlib.h`, `utils/dsp.h`, `utils/random.{h,cc}`

`braids_upstream.cpp` compiles them as one unit (three file-local constants
clash between the two oscillator files and are renamed there).
`BraidsEngine.cpp` / `resample_taps.h` and `core/libs/Audio/play_memory_braids.cpp`
are the Tracker integration, ported from the binary patch in
[mranthropology/Tracker-Firmware-Mod](https://github.com/mranthropology/Tracker-Firmware-Mod).
