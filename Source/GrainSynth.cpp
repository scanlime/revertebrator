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
  if (controllerNumber == 1) {
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
