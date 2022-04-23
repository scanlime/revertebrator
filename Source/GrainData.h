#pragma once

#include <JuceHeader.h>

class GrainData : private juce::Value::Listener {
public:
  GrainData();
  ~GrainData() override;
  void startThread();

  juce::Value src, status;

  class Accessor {
  public:
    Accessor(GrainData &data);

    bool read(float *const *destChannels, int numDestChannels,
              juce::int64 startSampleInSource, int numSamplesToRead);

    int sampleRate() const;
    float maxGrainWidth() const;
    int numBins() const;
    int numGrains() const;
    juce::int64 numSamples() const;

    juce::int64 centerSampleForGrain(int grain) const;
    float pitchForBin(int bin) const;
    int closestBinForPitch(float hz) const;
    juce::Range<int> grainsForBin(int bin) const;

  private:
    GrainData &ref;
    juce::ScopedReadLock reader;
  };

private:
  void valueChanged(juce::Value &) override;
  void load(juce::String &);

  juce::TimeSliceThread loadingThread;

  struct State {
    juce::File srcFile, soundFile;
    std::unique_ptr<juce::BufferingAudioReader> reader;
    juce::int64 soundLen;
    float maxGrainWidth;
    int sampleRate;
    juce::Array<int> binX;
    juce::Array<float> binF0;
    juce::Array<juce::int64> grainX;

    juce::String toString() const;
    int numBins() const;
    int numGrains() const;
  };

  juce::ReadWriteLock rwLock;
  std::unique_ptr<State> state;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
