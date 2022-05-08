#pragma once

#include "GrainData.h"
#include <JuceHeader.h>
#include <random>

class GrainSequence {
public:
  struct Params {
    float selCenter, selMod, selSpread;
    float speedWarp, pitchSpread, pitchBendRange;
    float gainDbLow, gainDbHigh;
  };
  struct Midi {
    int note, pitchWheel, modWheel;
    float velocity;
  };
  struct Point {
    unsigned grain;
    float gain;
  };

  GrainSequence(GrainIndex &index, const Params &p, const Midi &m);
  ~GrainSequence();
  Point generate();

  GrainIndex::Ptr index;
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

  GrainIndex &getIndex();
  GrainWaveform::Key waveformForGrain(unsigned grain);
  std::unique_ptr<GrainSequence> grainSequence(const GrainSequence::Midi &);

private:
  GrainIndex::Ptr index;
  Params params;
  float speedRatio;
  GrainWaveform::Window window;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainSound)
};

class GrainVoice : public juce::SynthesiserVoice {
public:
  GrainVoice(GrainData &grainData);
  ~GrainVoice() override;

  bool canPlaySound(juce::SynthesiserSound *) override;
  void startNote(int, float, juce::SynthesiserSound *, int) override;
  void stopNote(float, bool) override;
  void pitchWheelMoved(int) override;
  void controllerMoved(int, int) override;
  void renderNextBlock(juce::AudioBuffer<float> &, int, int) override;

private:
  GrainData &grainData;
  std::unique_ptr<GrainSequence> sequence;
  std::deque<GrainSequence::Point> queue;
  int temp_sample{0};
  float temp_gain{0.f};
  GrainWaveform::Ptr temp_wave;
  int currentModWheelPosition{0};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainVoice)
};

class GrainSynth : public juce::Synthesiser {
public:
  GrainSynth(GrainData &grainData, int numVoices);
  ~GrainSynth() override;

  void changeSound(GrainIndex &, const GrainSound::Params &);

  void noteOn(int, int, float) override;
  void handleController(int, int, int) override;

private:
  int lastModWheelValues[16];

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainSynth)
};
