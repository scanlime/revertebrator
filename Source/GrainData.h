#pragma once

#include <JuceHeader.h>

class GrainData : private juce::Value::Listener {
public:
  GrainData();
  ~GrainData() override;

  juce::Value src, status;

  class Accessor {
  public:
    Accessor(GrainData &data);

    bool read(float *const *destChannels, int numDestChannels,
              juce::int64 startSampleInSource, int numSamplesToRead);

    int sampleRate();
    float maxGrainWidth();
    int numBins();
    int numGrains();

    juce::int64 centerSampleForGrain(int grain);
    float pitchForBin(int bin);
    int closestBinForPitch(float hz);
    juce::Range<int> grainsForBin(int bin);

  private:
    GrainData &ref;
    juce::ScopedReadLock reader;
  };

private:
  void valueChanged(juce::Value &) override;
  void load(juce::String &);

  struct State {
    juce::File srcFile, soundFile;
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::int64 soundLen;
    float maxGrainWidth;
    int sampleRate;
    juce::Array<int> binX;
    juce::Array<float> binF0;
    juce::Array<juce::int64> grainX;

    juce::String toString() const;
  };

  juce::ReadWriteLock rwLock;
  std::unique_ptr<State> state;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
