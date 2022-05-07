#pragma once

#include "GrainData.h"
#include <JuceHeader.h>
#include <random>

class GrainSequence {
public:
  struct Params {
    float selCenter, selMod, selSpread;
    float speedWarp, pitchSpread;
  };
  struct Midi {
    int midiNote, pitchWheel, modWheel;
    float velocity;
  };
  struct Point {
    unsigned grain;
    float gain;
  };

  inline GrainSequence(const Params &p, const Midi &m) : params(p), midi(m) {}
  inline ~GrainSequence() {}
  Point generate();

  Params params;
  Midi midi;

private:
  std::mt19937 prng;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainSequence)
};

class GrainSound : public juce::SynthesiserSound {
public:
  struct Params {
    double sampleRate, grainRate;
    GrainWaveform::Window::Params window;
    GrainSequence::Params sequence;
  };

  GrainSound(GrainIndex &index, const Params &params);
  ~GrainSound() override;
  bool appliesToNote(int) override;
  bool appliesToChannel(int) override;

private:
  friend class GrainVoice;
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
  std::unique_ptr<GrainSequence> sequence;
  std::deque<GrainSequence::Point> queue;
  int currentModWheelPosition{0};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainVoice)
};
