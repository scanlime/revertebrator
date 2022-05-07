#include "GrainSynth.h"

GrainSound::GrainSound(GrainIndex &index, const Params &params)
    : index(index), params(params),
      speedRatio(index.sampleRate / params.sampleRate * params.speed_warp),
      window(index.maxGrainWidthSamples() / speedRatio, params.window) {}

GrainSound::~GrainSound() {}
bool GrainSound::appliesToNote(int) { return true; }
bool GrainSound::appliesToChannel(int) { return true; }

GrainVoice::GrainVoice() {}
GrainVoice::~GrainVoice() {}

bool GrainVoice::canPlaySound(juce::SynthesiserSound *sound) {
  return dynamic_cast<GrainSound *>(sound) != nullptr;
}

void GrainVoice::startNote(int, float, juce::SynthesiserSound *, int) {}

void GrainVoice::stopNote(float, bool) {}

void GrainVoice::pitchWheelMoved(int){};
void GrainVoice::controllerMoved(int, int) {}

void GrainVoice::renderNextBlock(juce::AudioBuffer<float> &, int, int) {}
