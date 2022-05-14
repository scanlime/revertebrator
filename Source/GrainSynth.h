#pragma once

#include "GrainData.h"
#include <JuceHeader.h>
#include <deque>
#include <random>

class GrainSequence {
public:
  using Ptr = std::unique_ptr<GrainSequence>;

  struct Point {
    unsigned grain;
    float gain;
  };

  GrainIndex::Ptr index;

  GrainSequence(GrainIndex &);
  virtual ~GrainSequence();
  virtual Point generate(std::mt19937 &) = 0;
};

class StationaryGrainSequence : public GrainSequence {
public:
  StationaryGrainSequence(GrainIndex &, const Point &);
  ~StationaryGrainSequence() override;
  Point generate(std::mt19937 &) override;

private:
  Point value;
};

class MidiGrainSequence : public GrainSequence {
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

  Params params;
  Midi midi;

  MidiGrainSequence(GrainIndex &, const Params &, const Midi &);
  ~MidiGrainSequence() override;
  Point generate(std::mt19937 &) override;
};

class GrainSound : public juce::SynthesiserSound {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainSound>;

  struct Params {
    double sampleRate, grainRate;
    GrainWaveform::Window::Params window;
    MidiGrainSequence::Params sequence;
  };

  GrainSound(GrainIndex &index, const Params &params);
  ~GrainSound() override;
  bool appliesToNote(int) override;
  bool appliesToChannel(int) override;

  GrainIndex &getIndex();
  bool isUsingSameIndex(GrainIndex &ix) const;
  double grainRepeatsPerSample() const;
  double outputSampleRate() const;
  int targetQueueDepth() const;
  float maxGrainWidthSamples() const;
  const GrainWaveform::Window &getWindow() const;
  GrainWaveform::Key waveformKeyForGrain(unsigned grain) const;
  GrainSequence::Ptr grainSequence(const MidiGrainSequence::Midi &midi);
  GrainSequence::Ptr grainSequence(unsigned grain, float velocity);

private:
  GrainIndex::Ptr index;
  Params params;
  float speedRatio;
  float maxWidthSamples;
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
  bool isVoiceActive() const override;
  void pitchWheelMoved(int) override;
  void controllerMoved(int, int) override;
  void renderNextBlock(juce::AudioBuffer<float> &, int, int) override;

  class Listener {
  public:
    virtual void grainVoicePlaying(const GrainVoice &voice,
                                   const GrainSound &sound, GrainWaveform &wave,
                                   const GrainSequence::Point &seq,
                                   int sampleNum, int sampleCount) = 0;
  };

  void addListener(Listener *);
  void removeListener(Listener *);

  void startGrain(unsigned grain, float velocity);

  void clearGrainQueue();
  void replaceReservoirWithQueuedGrains();

private:
  struct Grain {
    GrainSequence::Point seq;
    GrainWaveform::Ptr wave;
  };

  struct Reservoir {
    std::vector<Grain> grains;
    juce::SortedSet<unsigned> set;

    void add(const Grain &);
    void clear();
  };

  void fillQueueForSound(const GrainSound &);
  void fetchQueueWaveforms(GrainSound &);
  int numActiveGrainsInQueue(const GrainSound &);
  void trimQueueToLength(int);
  void trimAndRefillQueue(int);
  void renderFromQueue(const GrainSound &, juce::AudioBuffer<float> &, int,
                       int);

  GrainData &grainData;

  std::mutex listenerMutex;
  juce::ListenerList<Listener> listeners;

  std::mt19937 prng;
  GrainSequence::Ptr sequence;
  std::deque<Grain> queue;
  Reservoir reservoir;

  int sampleOffsetInQueue{0};
  int currentModWheelPosition{0};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainVoice)
};

class GrainSynth : public juce::Synthesiser {
public:
  GrainSynth(GrainData &grainData, int numVoices);
  ~GrainSynth() override;

  void changeSound(GrainIndex &, const GrainSound::Params &);
  GrainSound::Ptr latestSound();

  void mouseInputForGrain(unsigned grainId, bool isDown, int sourceId);

  void addListener(GrainVoice::Listener *);
  void removeListener(GrainVoice::Listener *);

  void noteOn(int, int, float) override;
  void handleController(int, int, int) override;

private:
  int lastModWheelValues[16];
  juce::HashMap<int, GrainVoice *> voiceForInputSource;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainSynth)
};
