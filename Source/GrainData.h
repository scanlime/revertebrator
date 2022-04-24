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

    int sampleRate() const { return ref.state->sampleRate; }
    float maxGrainWidth() const { return ref.state->maxGrainWidth; }
    int numBins() const { return ref.state->numBins(); }
    int numGrains() const { return ref.state->numGrains(); }
    juce::int64 numSamples() const { return ref.state->soundLen; }

    juce::int64 centerSampleForGrain(int grain) const {
      return ref.state->grainX[juce::jlimit(0, numGrains() - 1, grain)];
    }

    int closestBinForPitch(float hz) const;

    float pitchForBin(int bin) const {
      return ref.state->binF0[juce::jlimit(0, numBins() - 1, bin)];
    }

    juce::Range<int> grainsForBin(int bin) const {
      bin = juce::jlimit(0, numBins() - 1, bin);
      return juce::Range<int>(ref.state->binX[bin], ref.state->binX[bin + 1]);
    }

  private:
    GrainData &ref;
    juce::ScopedReadLock reader;
  };

private:
  void valueChanged(juce::Value &) override;
  void load(juce::String &);

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

    int numGrains() const { return grainX.size(); }
    int numBins() const {
      return juce::jmin(binF0.size(), binX.size(), binX.size() - 1);
    }
  };

  juce::TimeSliceThread loadingThread;
  juce::ReadWriteLock stateMutex;
  std::unique_ptr<State> state;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
