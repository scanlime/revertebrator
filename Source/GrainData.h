#pragma once

#include <JuceHeader.h>
#include <mutex>

class GrainWaveform : public juce::ReferenceCountedObject {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainWaveform>;

  struct Window {
    float mix;
    int width0, width1, phase1;

    struct Params {
      float mix, width0, width1, phase1;
    };

    inline Window(float maxWidthSamples, const Params &p)
        : mix(juce::jlimit(0.f, 1.f, p.mix)),
          width0(1 + std::round(juce::jlimit(0.f, 1.f, p.width0) *
                                (maxWidthSamples - 1.f))),
          width1(width0 + std::round(juce::jlimit(0.f, 1.f, p.width1) *
                                     (maxWidthSamples - float(width0)))),
          phase1(std::round(juce::jlimit(-1.f, 1.f, p.phase1) *
                            (maxWidthSamples - float(width1)))) {
      jassert(mix >= 0.f && mix <= 1.f);
      jassert(width0 >= 1 && width0 <= std::ceil(maxWidthSamples));
      jassert(width1 >= width0 && width1 <= std::ceil(maxWidthSamples));
      jassert(std::abs(phase1) <= std::ceil(maxWidthSamples));
    };

    inline float evaluate(float x) const noexcept {
      float ph0 = x / width0;
      float ph1 = (x - phase1) / width1;
      float win0 = 0.5 + 0.5 * cosf(juce::jlimit(-1.f, 1.f, ph0) * M_PI);
      float win1 = 0.5 + 0.5 * cosf(juce::jlimit(-1.f, 1.f, ph1) * M_PI);
      return win0 * (1.f - mix) + win1 * mix;
    }

    inline bool operator==(const Window &o) const noexcept {
      return mix == o.mix && width0 == o.width0 && width1 == o.width1 &&
             phase1 == o.phase1;
    }

    inline juce::Range<int> rangeW0() const noexcept {
      return juce::Range<int>(-width0, width0);
    }

    inline juce::Range<int> rangeW1() const noexcept {
      return juce::Range<int>(-width1, width1) + phase1;
    }

    inline juce::Range<int> range() const noexcept {
      return rangeW0().getUnionWith(rangeW1());
    }
  };

  struct Key {
    unsigned grain;
    float speedRatio;
    Window window;

    inline bool operator==(const Key &o) const noexcept {
      return grain == o.grain && speedRatio == o.speedRatio &&
             window == o.window;
    }
  };

  GrainWaveform(const Key &, int channels, int samples);
  ~GrainWaveform() override;

  inline bool isEmpty() const noexcept { return buffer.getNumSamples() == 0; }

  Key key;
  juce::AudioBuffer<float> buffer;

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainWaveform)
};

class GrainIndex : public juce::ReferenceCountedObject {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainIndex>;

  GrainIndex(const juce::File &);
  ~GrainIndex() override;

  juce::File file;
  float sampleRate{0}, maxGrainWidth{0};
  juce::int64 numSamples{0};
  juce::Range<juce::int64> soundFileBytes;
  juce::Array<unsigned> binX;
  juce::Array<float> binF0;
  juce::Array<juce::uint64> grainX;
  juce::Result status;

  inline unsigned numBins() const { return binF0.size(); }
  inline unsigned numGrains() const { return grainX.size(); }

  inline bool isValid() const {
    return status.wasOk() && numBins() && numGrains() && numSamples;
  }

  inline float maxGrainWidthSamples() const {
    return sampleRate * maxGrainWidth;
  }

  inline unsigned closestBinForPitch(float hz) const {
    auto x = std::lower_bound(binF0.begin(), binF0.end(), hz) - binF0.begin();
    if (x > 0) {
      return (fabs(binF0[x - 1] - hz) < fabs(binF0[x] - hz)) ? (x - 1) : x;
    } else {
      return 0;
    }
  }

  inline juce::Range<float> pitchRange() const {
    return juce::Range<float>(binF0[0], binF0[numBins() - 1]);
  }

  inline juce::Range<unsigned> grainsForBin(unsigned bin) const {
    return juce::Range<unsigned>(binX[bin], binX[bin + 1]);
  }

  juce::String describeToString() const;
  void cacheWaveform(GrainWaveform &);
  GrainWaveform::Ptr getCachedWaveformOrInsertEmpty(const GrainWaveform::Key &);

  class Listener {
  public:
    virtual void grainIndexWaveformStored(const GrainWaveform::Key &);
    virtual void grainIndexWaveformVisited(const GrainWaveform::Key &);
    virtual void grainIndexWaveformMissing(const GrainWaveform::Key &);
  };

  void addListener(Listener *);
  void removeListener(Listener *);

private:
  struct Hasher {
    int generateHash(const GrainWaveform::Window &, int) const noexcept;
    int generateHash(const GrainWaveform::Key &, int) const noexcept;
  };

  std::mutex cacheMutex;
  juce::HashMap<GrainWaveform::Key, GrainWaveform::Ptr, Hasher> cache;

  std::mutex listenerMutex;
  juce::ListenerList<Listener> listeners;

  juce::Result load();

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainIndex)
};

class GrainData {
public:
  GrainData(juce::ThreadPool &generalPurposeThreads);
  virtual ~GrainData();

  void referFileInputTo(const juce::Value &);
  void referToStatusOutput(juce::Value &);

  GrainIndex::Ptr getIndex();
  GrainWaveform::Ptr getWaveform(GrainIndex &, const GrainWaveform::Key &);

private:
  class IndexLoaderJob;
  class WaveformLoaderThread;

  juce::Atomic<int> waveformThreadSequence;
  juce::OwnedArray<WaveformLoaderThread> waveformLoaderThreads;
  std::unique_ptr<IndexLoaderJob> indexLoaderJob;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
