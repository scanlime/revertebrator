#pragma once

#include "GrainData.h"
#include <JuceHeader.h>
#include <deque>
#include <random>

class GrainSequence {
public:
  using Ptr = std::unique_ptr<GrainSequence>;
  using Rng = std::mt19937;

  struct Point {
    GrainWaveform::Key waveKey;
    float gain;
    int samplesUntilNextPoint;
  };

  struct Params {
    GrainWaveform::Window::Params windowParams;
    float sampleRate, grainRate, grainRateSpread;
    float selSpread, pitchSpread;
    float speedWarp, gainDbLow, gainDbHigh;

    float speedRatio(const GrainIndex &) const;
    GrainWaveform::Window window(const GrainIndex &index) const;
    unsigned chooseGrain(const GrainIndex &, float pitch, float sel);
    float velocityToGain(float velocity) const;
    int samplesUntilNextPoint(Rng &) const;
    float selNoise(Rng &, float) const;
    float pitchNoise(Rng &, float) const;
  };

  virtual ~GrainSequence();
  virtual Point generate(Rng &) = 0;
};

class TouchGrainSequence : public GrainSequence {
public:
  struct Event {
    float pitch, sel, velocity;
  };

  TouchGrainSequence(GrainIndex &, const Params &, const Event &);
  ~TouchGrainSequence() override;
  Point generate(Rng &) override;

private:
  GrainIndex &index;
  Params params;
  Event event;
};

class MidiGrainSequence : public GrainSequence {
public:
  struct MidiParams {
    Params common;
    float selCenter, selMod, pitchBendRange;
  };

  struct MidiState {
    int note, pitchWheel, modWheel;
    float velocity;
  };

  MidiGrainSequence(GrainIndex &, const MidiParams &, const MidiState &);
  ~MidiGrainSequence() override;
  Point generate(Rng &) override;

private:
  GrainIndex &index;
  MidiParams params;
  MidiState state;
};

class GrainSound : public juce::SynthesiserSound {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainSound>;

  GrainSound(GrainIndex &index, const MidiGrainSequence::MidiParams &params);
  ~GrainSound() override;
  bool appliesToNote(int) override;
  bool appliesToChannel(int) override;

  GrainIndex::Ptr index;
  MidiGrainSequence::MidiParams params;

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainSound)
};

class GrainVoice : public juce::SynthesiserVoice {
public:
  GrainVoice(GrainData &, const std::mt19937 &);
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
    virtual void grainVoicePlaying(const GrainVoice &, const GrainSound &,
                                   GrainWaveform &,
                                   const GrainSequence::Point &, int sampleNum,
                                   int sampleCount) = 0;
  };

  void addListener(Listener *);
  void removeListener(Listener *);

  void clearGrainQueue();
  void replaceReservoirWithQueuedGrains();

private:
  struct Grain {
    GrainSequence::Point seq;
    GrainWaveform::Ptr wave;
  };

  class Reservoir {
  public:
    Reservoir();
    void add(const Grain &);
    void clear();
    bool empty() const;
    const Grain &choose(std::mt19937 &) const;

  private:
    std::deque<Grain> grains;
    juce::SortedSet<unsigned> set;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Reservoir);
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
