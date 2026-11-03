// Braids engine in the sample player (original Tracker, FEATURE_BRAIDS).
//
// An instrument named "BR-<model>" plays Mutable Instruments Braids instead of
// its sample. The stock note-on for the instrument's play mode still runs (it
// sets pitch, tune, finetune, glide, envelope and the granular parameters);
// braidsNoteOn() then picks the model and takes one of braidsEngine::kMaxVoices
// voices, and update() renders Braids into the block before the envelope, so
// the amp envelope, filter, bit depth, sends, volume and pan all apply.
//   pitch  <- pitchControl (note, tune, finetune, glide, pitch FX)
//   Timbre <- Granular Position (live, modulated) in Granular mode, else the
//             instrument's static position
//   Color  <- Granular Length (live, modulated) in Granular mode, else static
#include "play_memory.h"

#ifdef FEATURE_BRAIDS

#include "BraidsEngine.h"

void AudioPlayMemory::braidsNoteOn(uint8_t instrIdx)
{
    int model = braidsEngine::parseModel(mtProject.instrument[instrIdx].sample.file_name);
    if (model < 0)
    {
        braidsRelease();
        return;
    }
    int voice = braidsEngine::acquire(this);
    if (voice < 0)
    {
        braidsModel = -1; // all voices busy: play the instrument's sample
        return;
    }
    braidsVoice = static_cast<int8_t>(voice);
    braidsModel = static_cast<int8_t>(model);
    braidsEngine::noteOn(voice, model);
}

void AudioPlayMemory::braidsRelease()
{
    if (braidsVoice >= 0)
    {
        braidsEngine::release(braidsVoice);
    }
    braidsVoice = -1;
    braidsModel = -1;
}

AUDIO_CORE_FLASH audio_block_t* AudioPlayMemory::updateBraids()
{
    if (braidsVoice < 0)
        return nullptr;
    audio_block_t* block = allocate();
    if (!block)
        return nullptr;

    // Glide: the sample modes step pitchControl once per output sample.
    if (constrainsInSamples.pitchSmoothing)
    {
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
        {
            if (pitchSmoothingControlCounter > constrainsInSamples.pitchSmoothing)
                break;
            pitchControl += pitchSmoothingControl;
            if (pitchControl > notes[MAX_NOTE - 1])
            {
                pitchControl = notes[MAX_NOTE - 1];
                pitchSmoothingControlCounter = constrainsInSamples.pitchSmoothing;
            }
            else if (pitchControl < notes[MIN_NOTE])
            {
                pitchControl = notes[MIN_NOTE];
                pitchSmoothingControlCounter = constrainsInSamples.pitchSmoothing;
            }
            pitchSmoothingControlCounter++;
        }
    }

    // notes[60] is C5 including the live clock correction, so the ratio is
    // 1.0 at C5 both live and in offline export.
    int pitch = braidsEngine::pitchFromRate(pitchControl / notes[60]);

    const strInstrument& instr = mtProject.instrument[currentInstrIdx];
    const bool granular = currentPlayMode == playModeGranular;
    uint16_t position = granular ? currentGranularPosition : instr.granular.currentPosition;
    uint16_t length = granular ? currentGranularLength : instr.granular.grainLength;

    braidsEngine::render(braidsVoice,
                         braidsModel,
                         pitch,
                         position >> 1,
                         braidsEngine::colorFromGrainLength(length),
                         block->data);
    return block;
}

#endif // FEATURE_BRAIDS
