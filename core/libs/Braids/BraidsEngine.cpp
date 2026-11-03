// Braids as a Tracker instrument engine (original Tracker). Ported from the
// fw 1.9.2 binary patch (Tracker-Firmware-Mod, braids/braids_voice.cc and
// tracker_voice.cc); Braids itself is the unmodified upstream in upstream/.
#include "BraidsEngine.h"

#ifdef FEATURE_BRAIDS

#include <arm_acle.h>
#include <cstdio>
#include <cstring>
#include <new>

#include "braids/macro_oscillator.h"
#include "cr_section_macros.h"
#include "resample_taps.h"

using braids::MacroOscillator;
using braids::MacroOscillatorShape;

namespace
{
// Braids' tables are for 96 kHz. Rendering directly at the Tracker's
// 44,117.647 Hz needs the pitch raised by 12 * log2(96000 / 44117.647)
// semitones (Braids pitch is in 1/128 semitone).
constexpr int16_t kPitchOffset = 1723;
constexpr int kChunk = 24;    // Braids' internal render size on the module

// 96 kHz path state, kept right after the oscillator in a voice slot.
struct ResampleState
{
    int16_t history[RESAMPLE_TAPS - 1];   // last inputs, oldest first
    uint16_t frac;                        // position within an input sample, 1/125ths
    int16_t pending;                      // rendered sample not yet consumed
    uint16_t hasPending;
};

struct VoiceSlot
{
    alignas(8) uint8_t osc[sizeof(MacroOscillator)];
    ResampleState resample;
};

// SDRAM (not zeroed at boot): every field is set before use.
__NOINIT(EXTERNAL_RAM) VoiceSlot voiceSlots[braidsEngine::kMaxVoices];

struct VoiceInfo
{
    const void* owner = nullptr;
    int8_t model = -1;          // model the oscillator was initialised for
    bool at96k = false;         // last block went through the 96 kHz path
};
VoiceInfo voiceInfo[braidsEngine::kMaxVoices];

// Load guard state (see audioUpdateDone). Level 0: normal; 1: no 96 kHz path;
// 2..kMaxVoices+1: that many minus one voices silenced, costliest first.
constexpr int kMaxShedLevel = braidsEngine::kMaxVoices + 1;
constexpr int kShedPercent = 95;          // smoothed load that sheds a step
constexpr int kRestorePercent = 90;       // predicted load a restore must fit under
constexpr int kClampPercent = 110;        // one block's most weight in the average (spikes)
constexpr int kHoldBlocks = 16;           // settle time after any change
constexpr int kRestoreBlocks = 344;       // ~1 s of fitting before a restore
constexpr int kMaxRestoreBlocks = 344 * 16;
constexpr int kStableBlocks = 344 * 10;   // a restore that lasts this long resets the backoff
constexpr int kCyclesPerUs = 180;
// Extra cost of the 96 kHz path over direct rendering, x4 (measured 2.9-3.7x
// in total, so the extra is ~2-2.7x the direct cost).
constexpr uint32_t k96kExtraX4 = 11;
int shedLevel = 0;
int smoothedUs = 0;                       // load, exponential average over ~8 blocks
int holdBlocks = 0;
int fitBlocks = 0;
int restoreBlocks = kRestoreBlocks;       // grows when a restore is undone quickly
int sinceRestore = 0;                     // blocks since the last restore, -1: none pending
uint32_t voiceCycles[braidsEngine::kMaxVoices];   // last render cost per voice

inline uint32_t cycleCount()
{
#ifdef BRAIDS_HOST_TEST
    return 0;
#else
    return *reinterpret_cast<volatile uint32_t*>(0xE0001004);   // DWT->CYCCNT
#endif
}

// Silenced at the current level: among the voices in use, this one is one of
// the (shedLevel - 1) costliest by last measured render.
bool isSilenced(int voice)
{
    int silenced = shedLevel - 1;
    if (silenced <= 0)
        return false;
    int costlier = 0;
    for (int v = 0; v < braidsEngine::kMaxVoices; ++v)
    {
        if (v == voice || !voiceInfo[v].owner)
            continue;
        if (voiceCycles[v] > voiceCycles[voice] || (voiceCycles[v] == voiceCycles[voice] && v > voice))
            ++costlier;
    }
    return costlier < silenced;
}

bool rendersAt96k(int model);

bool any96kVoice()
{
    for (int v = 0; v < braidsEngine::kMaxVoices; ++v)
        if (voiceInfo[v].owner && rendersAt96k(voiceInfo[v].model))
            return true;
    return false;
}

// Cycles the next restore step would add: the 96 kHz path's extra for the
// voices using it (level 1), or the cheapest silenced voice (higher levels).
uint32_t restoreCostCycles()
{
    uint32_t cost = 0;
    for (int v = 0; v < braidsEngine::kMaxVoices; ++v)
    {
        if (!voiceInfo[v].owner)
            continue;
        if (shedLevel == 1)
        {
            if (rendersAt96k(voiceInfo[v].model))
                cost += voiceCycles[v] * k96kExtraX4 / 4;
        }
        else if (isSilenced(v) && (cost == 0 || voiceCycles[v] < cost))
            cost = voiceCycles[v];
    }
    return cost;
}

const char* const kModelNames[braidsEngine::kNumModels] = {
    "CSAW", "MORPH", "SAWSQUARE", "SINETRIANGLE", "BUZZ", "SQUARESUB", "SAWSUB",
    "SQUARESYNC", "SAWSYNC", "TRIPLESAW", "TRIPLESQUARE", "TRIPLETRIANGLE",
    "TRIPLESINE", "TRIPLERINGMOD", "SAWSWARM", "SAWCOMB", "TOY", "DIGITALFILTERLP",
    "DIGITALFILTERPK", "DIGITALFILTERBP", "DIGITALFILTERHP", "VOSIM", "VOWEL",
    "VOWELFOF", "HARMONICS", "FM", "FEEDBACKFM", "CHAOTICFEEDBACKFM", "PLUCKED",
    "BOWED", "BLOWN", "FLUTED", "STRUCKBELL", "STRUCKDRUM", "KICK", "CYMBAL",
    "SNARE", "WAVETABLES", "WAVEMAP", "WAVELINE", "WAVEPARAPHONIC", "FILTEREDNOISE",
    "TWINPEAKSNOISE", "CLOCKEDNOISE", "GRANULARCLOUD", "PARTICLENOISE",
    "DIGITALMODULATION", "QUESTIONMARK",
};

inline MacroOscillator* oscillator(int voice)
{
    return reinterpret_cast<MacroOscillator*>(voiceSlots[voice].osc);
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

// Models whose decays are per-sample constants: rendered at 96 kHz and
// resampled (struck bell/drum ~2.15x, snare 1.4x, particle noise ~1.5x too
// long when rendered directly at 44.1 kHz; measured in the binary patch's m16).
bool rendersAt96k(int model) { return model == 32 || model == 33 || model == 36 || model == 45; }

// log2 for x > 0: exponent from the float bits; log(1+m) = 2*atanh(m/(m+2)).
float log2Approx(float x)
{
    union
    {
        float f;
        uint32_t u;
    } v = { x };
    int e = static_cast<int>((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;   // mantissa in [1, 2)
    float m = v.f - 1.0f;
    float t = m / (m + 2.0f);
    float t2 = t * t;
    float ln = 2.0f * t * (1.0f + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 * (1.0f / 9)))));
    return static_cast<float>(e) + ln * 1.44269504f;
}

void renderChunks(MacroOscillator* o, int16_t* out, int n)
{
    uint8_t sync[kChunk];
    memset(sync, 0, sizeof(sync));
    while (n > 0)
    {
        int c = n > kChunk ? kChunk : n;
        o->Render(sync, out, c);
        out += c;
        n -= c;
    }
}

// Two adjacent int16 inputs as one word (the window is only 2-byte aligned;
// the M4's plain LDR accepts unaligned normal memory, but the compiler would
// split it into two halfword loads).
inline uint32_t loadPair(const int16_t* p)
{
    uint32_t v;
    __asm__("ldr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

// Braids at its native 96 kHz, resampled to 44,117.647 Hz by a 125-phase x
// 32-tap polyphase low-pass (272/125). n <= 128.
void render96k(int voice, int16_t* out, int n)
{
    MacroOscillator* o = oscillator(voice);
    ResampleState* st = &voiceSlots[voice].resample;
    constexpr int kHist = RESAMPLE_TAPS - 1;
    uint32_t total = st->frac + static_cast<uint32_t>(n) * RESAMPLE_STEP;
    int nIn = static_cast<int>(total / RESAMPLE_PHASES);
    int16_t buf[kHist + (127 * RESAMPLE_STEP + RESAMPLE_STEP + RESAMPLE_PHASES) / RESAMPLE_PHASES + 2];
    memcpy(buf, st->history, sizeof(st->history));
    // Braids' percussion models render two samples per loop step and wrap on
    // odd sizes, so only ever ask for an even count and carry a spare sample.
    int pos = 0;
    if (st->hasPending)
    {
        buf[kHist] = st->pending;
        st->hasPending = 0;
        pos = 1;
    }
    int rem = nIn - pos;
    if (rem > 0)
    {
        int even = (rem + 1) & ~1;
        renderChunks(o, buf + kHist + pos, even);
        if (even != rem)
        {
            st->pending = buf[kHist + nIn];
            st->hasPending = 1;
        }
    }
    uint32_t u = st->frac;
    for (int m = 0; m < n; ++m)
    {
        u += RESAMPLE_STEP;
        int k = static_cast<int>(u / RESAMPLE_PHASES);   // newest input used
        const uint32_t* h = reinterpret_cast<const uint32_t*>(kResampleTaps[u % RESAMPLE_PHASES]);
        const int16_t* x = buf + kHist + k - (RESAMPLE_TAPS - 1);
        int32_t acc = 0;
        for (int t = 0; t < RESAMPLE_TAPS / 2; ++t)
            acc = __smlad(h[t], loadPair(x + 2 * t), acc);
        acc >>= 15;
        out[m] = static_cast<int16_t>(acc > 32767 ? 32767 : (acc < -32768 ? -32768 : acc));
    }
    st->frac = static_cast<uint16_t>(total % RESAMPLE_PHASES);
    memcpy(st->history, buf + nIn, sizeof(st->history));
}
} // namespace

namespace braidsEngine
{
int parseModel(const char* name)
{
    char sep = name[2];
    if (upper(name[0]) != 'B' || upper(name[1]) != 'R' || (sep != ':' && sep != '-'))
        return -1;
    char token[24];
    int n = 0;
    for (int i = 3; i < 32 && n < 23; ++i)
    {
        char c = upper(name[i]);
        if (c == 0)
            break;
        if (c == ' ' || c == '_' || c == '-')
            continue;
        token[n++] = c;
    }
    token[n] = 0;
    if (n == 0)
        return 0;
    if (token[0] >= '0' && token[0] <= '9')
    {
        int v = 0;
        for (int i = 0; i < n && token[i] >= '0' && token[i] <= '9'; ++i)
            v = v * 10 + token[i] - '0';
        return v < kNumModels ? v : 0;
    }
    for (int m = 0; m < kNumModels; ++m)
    {
        const char* s = kModelNames[m];
        int i = 0;
        while (s[i] && s[i] == token[i])
            ++i;
        if (!s[i] && !token[i])
            return m;
    }
    return 0;   // unknown name: first model
}

int acquire(const void* owner)
{
    int freeVoice = -1;
    for (int v = 0; v < kMaxVoices; ++v)
    {
        if (voiceInfo[v].owner == owner)
            return v;
        if (!voiceInfo[v].owner && freeVoice < 0)
            freeVoice = v;
    }
    if (freeVoice >= 0)
        voiceInfo[freeVoice].owner = owner;
    return freeVoice;
}

void release(int voice)
{
    if (voice >= 0 && voice < kMaxVoices)
        voiceInfo[voice].owner = nullptr;
}

void noteOn(int voice, int model)
{
    if (voice < 0 || voice >= kMaxVoices)
        return;
    if (voiceInfo[voice].model != model)
    {
        MacroOscillator* o = new (voiceSlots[voice].osc) MacroOscillator;
        o->Init();
        memset(&voiceSlots[voice].resample, 0, sizeof(ResampleState));
        voiceInfo[voice].model = static_cast<int8_t>(model);
    }
    oscillator(voice)->Strike();
}

void render(int voice, int model, int pitch, int timbre, int color, int16_t* out, bool shed)
{
    if (voice < 0 || voice >= kMaxVoices)
    {
        memset(out, 0, 128 * sizeof(int16_t));
        return;
    }
    if (shed && isSilenced(voice))
    {
        memset(out, 0, 128 * sizeof(int16_t));
        return;
    }
    uint32_t start = cycleCount();
    MacroOscillator* o = oscillator(voice);
    o->set_shape(static_cast<MacroOscillatorShape>(model));
    o->set_parameters(static_cast<int16_t>(timbre), static_cast<int16_t>(color));
    if (rendersAt96k(model) && !(shed && shedLevel >= 1))
    {
        if (!voiceInfo[voice].at96k)
        {
            memset(&voiceSlots[voice].resample, 0, sizeof(ResampleState));
            voiceInfo[voice].at96k = true;
        }
        o->set_pitch(static_cast<int16_t>(pitch));   // 96 kHz tables: no offset
        render96k(voice, out, 128);
    }
    else
    {
        o->set_pitch(static_cast<int16_t>(pitch + kPitchOffset));
        renderChunks(o, out, 128);
        voiceInfo[voice].at96k = false;
    }
    for (int i = 0; i < 128; ++i)
        out[i] = static_cast<int16_t>(out[i] >> 1);   // -6 dB
    voiceCycles[voice] = cycleCount() - start;
}

void audioUpdateDone(int updateUs, int periodUs)
{
    int clamp = periodUs * kClampPercent / 100;
    smoothedUs += ((updateUs > clamp ? clamp : updateUs) - smoothedUs) / 8;
    if (sinceRestore >= 0 && ++sinceRestore >= kStableBlocks)
    {
        restoreBlocks = kRestoreBlocks;   // the last restore held
        sinceRestore = -1;
    }
    if (holdBlocks > 0)
    {
        --holdBlocks;
        return;
    }
    if (smoothedUs * 100 > periodUs * kShedPercent)
    {
        fitBlocks = 0;
        if (shedLevel < kMaxShedLevel)
        {
            ++shedLevel;
            if (shedLevel == 1 && !any96kVoice())
                shedLevel = 2;   // nothing on the 96 kHz path to save
            holdBlocks = kHoldBlocks;
            if (sinceRestore >= 0 && sinceRestore < 2 * 344)   // restore undone within 2 s
                restoreBlocks = restoreBlocks * 2 > kMaxRestoreBlocks ? kMaxRestoreBlocks : restoreBlocks * 2;
            sinceRestore = -1;
        }
        return;
    }
    if (shedLevel == 0)
        return;
    int predicted = smoothedUs + static_cast<int>(restoreCostCycles() / kCyclesPerUs);
    if (predicted * 100 >= periodUs * kRestorePercent)
    {
        fitBlocks = 0;
        return;
    }
    if (++fitBlocks >= restoreBlocks)
    {
        --shedLevel;
        fitBlocks = 0;
        holdBlocks = kHoldBlocks;
        sinceRestore = 0;
    }
}

void describeLoad(char* text, int size)
{
    // "BR L1 47 20 18 -k": shed level, then each voice's last render in
    // thousand cycles ("." unused, "-" silenced).
    int n = snprintf(text, size, "BR L%d", shedLevel);
    for (int v = 0; v < kMaxVoices && n < size; ++v)
    {
        if (!voiceInfo[v].owner)
            n += snprintf(text + n, size - n, " .");
        else if (isSilenced(v))
            n += snprintf(text + n, size - n, " -");
        else
            n += snprintf(text + n, size - n, " %u", static_cast<unsigned>((voiceCycles[v] + 500) / 1000));
    }
    if (n < size)
        snprintf(text + n, size - n, "k");
}

int pitchFromRate(float rateOverC5)
{
    if (!(rateOverC5 > 0.0f))
        rateOverC5 = 1.0f;
    float note = 60.0f + 12.0f * log2Approx(rateOverC5);
    int pitch = static_cast<int>(note * 128.0f + 0.5f);
    return pitch < 0 ? 0 : (pitch > 127 * 128 ? 127 * 128 : pitch);
}

int16_t colorFromGrainLength(uint16_t samples)
{
    if (samples < 44)
        samples = 44;
    float c = log2Approx(samples / 44.0f) * (32767.0f / 9.96921f);   // log(len/44)/log(44100/44)
    return c > 32767.0f ? 32767 : static_cast<int16_t>(c);
}
} // namespace braidsEngine

#endif // FEATURE_BRAIDS
