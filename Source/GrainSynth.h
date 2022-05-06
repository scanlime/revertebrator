#pragma once

#include "GrainData.h"
#include <JuceHeader.h>

class GrainSound : public juce::SynthesiserSound {
public:
  struct Params {
    double sampleRate;
    float win_width0, win_width1, win_phase1, win_mix;
    float grain_rate, speed_warp, sel_center, sel_mod;
    float sel_spread, pitch_spread;
  };

  GrainSound(GrainIndex &index, const Params &params);
  ~GrainSound() override;
  bool appliesToNote(int) override;
  bool appliesToChannel(int) override;

private:
  GrainIndex::Ptr index;
  Params params;
  float speedRatio;
  GrainWaveform::Window window;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainSound)
};

class GrainVoice : public juce::SynthesiserVoice {
public:
  GrainVoice();
  ~GrainVoice() override;

  bool canPlaySound(juce::SynthesiserSound *) override;
  void startNote(int, float, juce::SynthesiserSound *, int) override;
  void stopNote(float, bool) override;
  void pitchWheelMoved(int) override;
  void controllerMoved(int, int) override;
  void renderNextBlock(juce::AudioBuffer<float> &, int, int) override;

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainVoice)
};
