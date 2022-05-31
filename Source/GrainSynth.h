#pragma once

#include "GrainData.h"
#include <JuceHeader.h>
#include <deque>
#include <random>

class GrainSequence {
public:
  using Ptr = std::unique_ptr<GrainSequence>;
  using Rng = std::mt19937;
  using Gains = std::array<float, 2>;

  struct Point {
    GrainWaveform::Key waveKey;
    int samplesUntilNextPoint;
    Gains gains;
  };

  struct Params {
    GrainWaveform::Window::Params windowParams;
    float sampleRate, grainRate, grainRateSpread;
    float selSpread, pitchSpread, stereoSpread;
    float speedWarp, stereoCenter, gainDbLow, gainDbHigh;
    float filterHighPass, filterLowPass;

    float speedRatio(const GrainIndex &, unsigned grain) const;
    float maxGrainWidthSamples(const GrainIndex &) const;
    GrainWaveform::Window window(const GrainIndex &) const;
    unsigned grain(const GrainIndex &, float &pitch, float &sel);
    Gains velocityToGains(Rng &, float) const;
    int samplesUntilNextPoint(Rng &) const;
    float selNoise(Rng &, float) const;
    float pitchNoise(Rng &, float) const;
    GrainWaveform::Filters filters(float pitch);
  };

  virtual ~GrainSequence();
  virtual Point generate(Rng &) = 0;
};

class TouchGrainSequence : public GrainSequence {
public:
  struct TouchEvent {
    float pitch, sel, velocity;
  };

  Params params;
  TouchEvent event;

  TouchGrainSequence(GrainIndex &, const Params &, const TouchEvent &);
  ~TouchGrainSequence() override;
  Point generate(Rng &) override;

private:
  GrainIndex &index;
};

class MidiGrainSequence : public GrainSequence {
public:
  struct MidiParams {
    Params common;
    float selCenter, selMod, pitchBendRange;
  };

  struct MidiEvent {
    int note, pitchWheel, modWheel;
    float velocity;
  };

  MidiParams params;
  MidiEvent event;

  MidiGrainSequence(GrainIndex &, const MidiParams &, const MidiEvent &);
  ~MidiGrainSequence() override;
  Point generate(Rng &) override;

private:
  GrainIndex &index;
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
  GrainVoice(GrainData &, const GrainSequence::Rng &);
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
                                   const GrainSequence::Point &,
                                   const juce::Range<int> &samples) = 0;
  };

  void addListener(Listener *);
  void removeListener(Listener *);
  void startTouch(const TouchGrainSequence::TouchEvent &event);
  void clearGrainQueue();

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
    const Grain &choose(GrainSequence::Rng &) const;

  private:
    std::deque<Grain> grains;
    std::unordered_set<GrainWaveform::Key, GrainWaveform::Hasher> set;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Reservoir);
  };

  void fillQueueForSound(const GrainSound &);
  void fetchQueueWaveforms(GrainSound &);
  int numActiveGrainsInQueue();
  void trimQueueToLength(int);
  void trimAndRefillQueue(int);
  void renderFromQueue(const GrainSound &, juce::AudioBuffer<float> &, int,
                       int);

  GrainData &grainData;

  std::mutex listenerMutex;
  juce::ListenerList<Listener> listeners;

  GrainSequence::Rng rng;
  GrainSequence::Ptr sequence;
  std::deque<Grain> queue;
  Reservoir reservoir;

  int sampleOffsetInQueue{0};
  int currentModWheelPosition{0};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainVoice)
};

class GrainSynth : public juce::Synthesiser {
public:
  struct TouchEvent {
    int sourceId;
    TouchGrainSequence::TouchEvent grain;
  };

  GrainSynth(GrainData &grainData, int numVoices);
  ~GrainSynth() override;

  void changeSound(GrainIndex &, const MidiGrainSequence::MidiParams &);
  GrainSound::Ptr latestSound();

  void touchEvent(const TouchEvent &);
  void addListener(GrainVoice::Listener *);
  void removeListener(GrainVoice::Listener *);

  void noteOn(int, int, float) override;
  void handleController(int, int, int) override;

private:
  int lastModWheelValues[16];
  juce::HashMap<int, GrainVoice *> voiceForInputSource;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainSynth)
};
