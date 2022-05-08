#pragma once

#include "GrainData.h"
#include <JuceHeader.h>
#include <random>

struct GrainSequence {
  using Ptr = std::unique_ptr<GrainSequence>;

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

  GrainIndex::Ptr index;
  Params params;
  Midi midi;

  Point generate(std::mt19937 &prng);
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
  double grainRepeatsPerSample() const;
  int windowSizeInSamples() const;
  GrainWaveform::Key waveformKeyForGrain(unsigned grain) const;
  GrainSequence::Ptr grainSequence(const GrainSequence::Midi &);

private:
  GrainIndex::Ptr index;
  Params params;
  float speedRatio;
  GrainWaveform::Window window;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainSound)
};

class GrainVoice : public juce::SynthesiserVoice {
public:
  GrainVoice(GrainData &grainData, const std::mt19937 &prng);
  ~GrainVoice() override;

  bool canPlaySound(juce::SynthesiserSound *) override;
  void startNote(int, float, juce::SynthesiserSound *, int) override;
  void stopNote(float, bool) override;
  void pitchWheelMoved(int) override;
  void controllerMoved(int, int) override;
  void renderNextBlock(juce::AudioBuffer<float> &, int, int) override;

private:
  struct Grain {
    GrainSequence::Point seq;
    GrainWaveform::Ptr wave;
  };

  void fillQueueToDepth(int numGrains);
  void fetchQueueWaveforms();
  int renderFromQueue(GrainSound &, juce::AudioBuffer<float> &, int, int);
  void advanceQueueBySamples(int numSamples);

  GrainData &grainData;
  std::mt19937 prng;
  std::unique_ptr<GrainSequence> sequence;
  std::deque<Grain> queue;
  int sampleOffsetInQueue{0};
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
