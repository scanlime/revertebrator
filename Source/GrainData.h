#pragma once

#include <JuceHeader.h>

struct GrainWindow {
  GrainWindow(float maxWidthSamples, float mix, float width0, float width1,
              float phase1);
  float mix;
  int width0, width1, phase1;
};

class GrainWaveId {
  unsigned grain;
  float speedRatio;
  GrainWindow window;
};

class GrainWaveData {};

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

    forcedinline unsigned sampleRate() const { return ref.state->sampleRate; }
    forcedinline juce::uint64 numSamples() const { return ref.state->soundLen; }
    forcedinline float maxGrainWidth() const {
      return ref.state->maxGrainWidth;
    }
    forcedinline unsigned numGrains() const { return ref.state->grainX.size(); }

    forcedinline unsigned numBins() const {
      return juce::jmin(ref.state->binF0.size(), ref.state->binX.size(),
                        ref.state->binX.size() - 1);
    }

    forcedinline juce::uint64 centerSampleForGrain(unsigned grain) const {
      if (grain < numGrains()) {
        return ref.state->grainX[grain];
      } else {
        return 0;
      }
    }

    forcedinline float pitchForBin(unsigned bin) const {
      if (bin < numBins()) {
        return ref.state->binF0[bin];
      } else {
        return 0.f;
      }
    }

    forcedinline juce::Range<float> pitchRange() const {
      if (numBins()) {
        return juce::Range<float>(ref.state->binF0[0],
                                  ref.state->binF0[numBins() - 1]);
      } else {
        return juce::Range<float>();
      }
    }

    forcedinline juce::Range<unsigned> grainsForBin(unsigned bin) const {
      if (bin < numBins()) {
        return juce::Range<unsigned>(ref.state->binX[bin],
                                     ref.state->binX[bin + 1]);
      } else {
        return juce::Range<unsigned>();
      }
    }

    forcedinline unsigned closestBinForPitch(float hz) const {
      auto begin = ref.state->binF0.begin(), end = ref.state->binF0.end();
      unsigned bin1 = std::lower_bound(begin, end, hz) - begin;
      unsigned bin0 = bin1 - 1;
      float dist0 = fabs(pitchForBin(bin0) - hz);
      float dist1 = fabs(pitchForBin(bin1) - hz);
      return (dist0 < dist1) ? bin0 : bin1;
    }

    juce::String describeToString() const;
    juce::String numSamplesToString() const;

  private:
    GrainData &ref;
    juce::ScopedReadLock reader;
  };

private:
  void valueChanged(juce::Value &) override;
  int useTimeSlice() override;
  juce::String load(const juce::File &);

  struct State {
    // what does this rwlock really represent? only changes of the entire state
    // during a reload? if we are dealing with asynchronous loading of graindata
    // constantly this is of limited use, i don't want to writelock every time a
    // grain is loaded. seems like we can replace this whole thing with a queue
    // pair and a thread pool.

    std::unique_ptr<juce::ZipFile> zip;
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::uint64 soundLen;
    float maxGrainWidth;
    unsigned sampleRate;
    juce::Array<unsigned> binX;
    juce::Array<float> binF0;
    juce::Array<juce::uint64> grainX;
  };

  juce::TimeSliceThread loadingThread;
  juce::String latestLoadingAttempt;

  juce::CriticalSection valuesMutex;
  juce::Value srcValue, statusValue;

  juce::ReadWriteLock stateMutex;
  std::unique_ptr<State> state;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
