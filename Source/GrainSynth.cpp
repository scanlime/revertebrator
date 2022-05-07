#include "GrainSynth.h"

GrainSequence::Point GrainSequence::generate() {
  return {grain : 0, gain : 1.0};
}

GrainSound::GrainSound(GrainIndex &index, const Params &params)
    : index(index), params(params),
      speedRatio(index.sampleRate / params.sampleRate *
                 params.sequence.speedWarp),
      window(index.maxGrainWidthSamples() / speedRatio, params.window) {}

GrainSound::~GrainSound() {}
bool GrainSound::appliesToNote(int) { return true; }
bool GrainSound::appliesToChannel(int) { return true; }
GrainVoice::GrainVoice() {}
GrainVoice::~GrainVoice() {}

bool GrainVoice::canPlaySound(juce::SynthesiserSound *sound) {
  return dynamic_cast<GrainSound *>(sound) != nullptr;
}

void GrainVoice::startNote(int midiNote, float velocity,
                           juce::SynthesiserSound *sound,
                           int currentPitchWheelPosition) {
  GrainSequence::Midi midi = {
      .midiNote = midiNote,
      .pitchWheel = currentPitchWheelPosition,
      .modWheel = currentModWheelPosition,
      .velocity = velocity,
  };
  auto grainSound = dynamic_cast<GrainSound *>(sound);
  if (grainSound != nullptr) {
    sequence =
        std::make_unique<GrainSequence>(grainSound->params.sequence, midi);
  }
}

void GrainVoice::stopNote(float, bool) { sequence = nullptr; }

void GrainVoice::pitchWheelMoved(int newValue) {
  if (sequence != nullptr) {
    sequence->midi.pitchWheel = newValue;
  }
}

void GrainVoice::controllerMoved(int controllerNumber, int newValue) {
  if (controllerNumber == 0x01) {
    currentModWheelPosition = newValue;
    if (sequence != nullptr) {
      sequence->midi.modWheel = newValue;
    }
  }
}

void GrainVoice::renderNextBlock(juce::AudioBuffer<float> &outputBuffer,
                                 int startSample, int numSamples) {
  if (sequence != nullptr) {
    auto p = sequence->generate();
    printf("render %p %d %d - %f %d %d - %d %f\n", this, startSample,
           numSamples, sequence->params.speedWarp, sequence->midi.modWheel,
           sequence->midi.pitchWheel, p.grain, p.gain);
  }
}

GrainSynth::GrainSynth(int numVoices) {
  for (auto i = 0; i < 16; i++) {
    lastModWheelValues[i] = 64;
  }
  for (auto i = 0; i < numVoices; i++) {
    addVoice(new GrainVoice());
  }
}

GrainSynth::~GrainSynth() {}

void GrainSynth::changeSound(GrainIndex &index,
                             const GrainSound::Params &params) {
  auto newSound = new GrainSound(index, params);
  juce::ScopedLock sl(lock);
  sounds.clear();
  sounds.add(newSound);
}

void GrainSynth::noteOn(int channel, int midiNote, float velocity) {
  juce::ScopedLock sl(lock);
  for (auto sound : sounds) {
    if (sound->appliesToNote(midiNote) && sound->appliesToChannel(channel)) {
      auto voice =
          findFreeVoice(sound, channel, midiNote, isNoteStealingEnabled());
      voice->controllerMoved(0x01, lastModWheelValues[channel]);
      startVoice(voice, sound, channel, midiNote, velocity);
    }
  }
}

void GrainSynth::handleController(int channel, int controller, int value) {
  if (controller == 0x01) {
    lastModWheelValues[channel] = value;
  }
  juce::Synthesiser::handleController(channel, controller, value);
}
