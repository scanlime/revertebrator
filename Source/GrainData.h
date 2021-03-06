#pragma once

#include <JuceHeader.h>

class GrainWaveform : public juce::ReferenceCountedObject {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainWaveform>;

  // Chain of IIR filters to apply prior to windowing and gain normalization
  using Filters = juce::Array<juce::IIRCoefficients>;

  // Window function, scaled to a particular size in samples
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

    inline float peakValue() const noexcept {
      auto y0 = evaluate(0);
      auto y1 = evaluate(phase1 / 2);
      auto y2 = evaluate(phase1);
      return std::max(y0, std::max(y1, y2));
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
    Filters filters;

    inline bool operator==(const Key &o) const noexcept {
      if (!(grain == o.grain && speedRatio == o.speedRatio &&
            window == o.window && filters.size() == o.filters.size())) {
        return false;
      }
      for (int f = 0; f < filters.size(); f++) {
        const auto &coeff = filters[f].coefficients;
        for (int c = 0; c < juce::numElementsInArray(coeff); c++) {
          if (coeff[c] != o.filters[f].coefficients[c]) {
            return false;
          }
        }
      }
      return true;
    }
  };

  struct Hasher {
    inline std::size_t operator()(Key const &key) const noexcept {
      return key.grain ^ int(key.speedRatio * 1e3) ^ int(key.window.mix * 3e3) ^
             (key.window.width0 * 2) ^ (key.window.width1 * 3) ^
             key.window.phase1;
    }
  };

  GrainWaveform(const Key &, int channels, int samples);
  ~GrainWaveform() override;

  inline bool isEmpty() const noexcept { return buffer.getNumSamples() == 0; }

  inline juce::int64 sizeInBytes() const noexcept {
    return buffer.getNumSamples() * buffer.getNumChannels() * sizeof(float);
  }

  Key key;
  juce::AudioBuffer<float> buffer;

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainWaveform)
};

class GrainWaveformCache {
public:
  class Listener {
  public:
    virtual void grainWaveformStored(const GrainWaveform::Key &) = 0;
    virtual void grainWaveformExpired(const GrainWaveform::Key &) = 0;
    virtual void grainWaveformLookup(const GrainWaveform::Key &,
                                     bool dataFound) = 0;
  };

  void addListener(Listener *);
  void removeListener(Listener *);

  juce::int64 sizeInBytes();
  void cleanup(int inactivityThreshold);
  void expire(const std::vector<GrainWaveform::Key> &);

  void store(GrainWaveform &);
  GrainWaveform::Ptr lookupOrInsertEmpty(const GrainWaveform::Key &);
  bool contains(const GrainWaveform::Key &);

private:
  struct Item {
    GrainWaveform::Ptr wave;
    int cleanupCounter{0};
  };

  std::mutex listenerMutex;
  juce::ListenerList<Listener> listeners;

  std::mutex cacheMutex;
  std::unordered_map<GrainWaveform::Key, Item, GrainWaveform::Hasher> map;
  juce::int64 totalBytes{0};
  int cleanupCounter{0};
};

class GrainSources {
public:
  void load(const juce::File &);
  juce::String pathForGrainSource(unsigned);

private:
  std::mutex mutex;
  juce::var data;
};

class GrainIndex : public juce::ReferenceCountedObject {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainIndex>;

  GrainIndex(const juce::File &);
  ~GrainIndex() override;

  juce::File file;
  float maxGrainWidth{0};
  juce::int64 numSamples{0};
  juce::Range<juce::int64> soundFileBytes;
  juce::Array<unsigned> binX;
  juce::Array<float> binF0;
  juce::Array<juce::uint64> grainX;
  juce::Array<float> sampleRates;
  juce::Result status;
  GrainWaveformCache cache;
  GrainSources sources;

  inline unsigned numBins() const { return binF0.size(); }
  inline unsigned numGrains() const { return grainX.size(); }

  inline bool isValid() const {
    return status.wasOk() && numBins() && numGrains() && numSamples;
  }

  inline unsigned closestBinForPitch(float hz) const {
    auto x = std::lower_bound(binF0.begin(), binF0.end(), hz) - binF0.begin();
    if (x > 0) {
      auto bin = (fabs(binF0[x - 1] - hz) < fabs(binF0[x] - hz)) ? (x - 1) : x;
      jassert(bin < numBins());
      return bin;
    } else {
      return 0;
    }
  }

  inline juce::Range<float> pitchRange() const {
    return juce::Range<float>(binF0[0], binF0[numBins() - 1]);
  }

  inline juce::Range<unsigned> grainsForBin(unsigned bin) const {
    jassert(bin < numBins());
    return juce::Range<unsigned>(binX[bin], binX[bin + 1]);
  }

  inline unsigned binForGrain(unsigned grain) const {
    jassert(grain < numGrains());
    auto x = -1 + std::max<int>(
                      1, std::lower_bound(binX.begin(), binX.end(), grain + 1) -
                             binX.begin());
    jassert(x < numBins());
    jassert(grainsForBin(x).contains(grain));
    return x;
  }

  juce::String describeToString() const;

private:
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
  float averageLoadQueueDepth();

private:
  class IndexLoaderJob;
  class CacheCleanupJob;
  class WaveformLoaderThread;

  juce::Atomic<int> waveformThreadSequence{0};
  juce::OwnedArray<WaveformLoaderThread> waveformLoaderThreads;
  std::unique_ptr<IndexLoaderJob> indexLoaderJob;
  std::unique_ptr<CacheCleanupJob> cacheCleanupJob;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
