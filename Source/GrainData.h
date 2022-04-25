#pragma once

#include <JuceHeader.h>

class GrainData : private juce::Value::Listener, private juce::TimeSliceClient {
public:
  GrainData();
  ~GrainData() override;

  void startThread();
  void stopThread(int timeOutMilliseconds);

  void referFileInputTo(const juce::Value &);
  void referToStatusOutput(juce::Value &);

  class Accessor {
  public:
    Accessor(GrainData &data);

    bool read(float *const *destChannels, int numDestChannels,
              juce::int64 startSampleInSource, int numSamplesToRead);

    unsigned sampleRate() const { return ref.state->sampleRate; }
    juce::uint64 numSamples() const { return ref.state->soundLen; }
    float maxGrainWidth() const { return ref.state->maxGrainWidth; }
    unsigned numGrains() const { return ref.state->grainX.size(); }

    unsigned numBins() const {
      return juce::jmin(ref.state->binF0.size(), ref.state->binX.size(),
                        ref.state->binX.size() - 1);
    }

    juce::uint64 centerSampleForGrain(unsigned grain) const {
      if (grain < numGrains()) {
        return ref.state->grainX[grain];
      } else {
        return 0;
      }
    }

    float pitchForBin(unsigned bin) const {
      if (bin < numBins()) {
        return ref.state->binF0[bin];
      } else {
        return 0.f;
      }
    }

    juce::Range<float> pitchRange() const {
      if (numBins()) {
        return juce::Range<float>(ref.state->binF0[0],
                                  ref.state->binF0[numBins() - 1]);
      } else {
        return juce::Range<float>();
      }
    }

    juce::Range<unsigned> grainsForBin(unsigned bin) const {
      if (bin < numBins()) {
        return juce::Range<unsigned>(ref.state->binX[bin],
                                     ref.state->binX[bin + 1]);
      } else {
        return juce::Range<unsigned>();
      }
    }

    juce::String describeToString() const;
    juce::String numSamplesToString() const;
    unsigned closestBinForPitch(float hz) const;

  private:
    GrainData &ref;
    juce::ScopedReadLock reader;
  };

private:
  void valueChanged(juce::Value &) override;
  int useTimeSlice() override;
  juce::String load(const juce::File &);

  struct State {
    juce::File srcFile, soundFile;
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::uint64 soundLen;
    float maxGrainWidth;
    unsigned sampleRate;
    juce::Array<unsigned> binX;
    juce::Array<float> binF0;
    juce::Array<juce::uint64> grainX;
  };

  juce::TimeSliceThread loadingThread;

  juce::CriticalSection valuesMutex;
  juce::Value srcValue, statusValue;

  juce::ReadWriteLock stateMutex;
  std::unique_ptr<State> state;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
