#pragma once

#include <JuceHeader.h>
#include <mutex>

class GrainWaveform : public juce::ReferenceCountedObject {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainWaveform>;

  struct Window {
    float mix;
    int width0, width1, phase1;
    Window(float maxWidthSamples, float mix, float w0, float w1, float p1);
    bool operator==(const Window &) noexcept;
  };

  struct Key {
    unsigned grain;
    float speedRatio;
    Window window;
    bool operator==(const Key &) noexcept;
  };

  GrainWaveform(const Key &, juce::uint64 grainX, juce::AudioFormatReader &);
  ~GrainWaveform() override;

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

  inline float maxGrainWidthSamples() const {
    return sampleRate * maxGrainWidth;
  }

  inline unsigned closestBinForPitch(float hz) const {
    auto x = std::lower_bound(binF0.begin(), binF0.end(), hz) - binF0.begin();
    return (fabs(binF0[x - 1] - hz) < fabs(binF0[x] - hz)) ? (x - 1) : x;
  }

  inline juce::Range<float> pitchRange() const {
    return juce::Range<float>(binF0[0], binF0[numBins() - 1]);
  }

  inline juce::Range<unsigned> grainsForBin(unsigned bin) const {
    return juce::Range<unsigned>(binX[bin], binX[bin + 1]);
  }

  GrainWaveform::Ptr getWaveform(const GrainWaveform::Key &);

  juce::String describeToString() const;
  static juce::String numSamplesToString(juce::uint64 numSamples);

private:
  struct Hasher {
    int generateHash(const GrainWaveform::Window &, int) noexcept;
    int generateHash(const GrainWaveform::Key &, int) noexcept;
  };

  std::mutex cacheMutex;
  juce::HashMap<GrainWaveform::Key, GrainWaveform::Ptr, Hasher> cache;

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

private:
  class IndexLoaderJob;
  class WaveformLoaderThread;

  juce::OwnedArray<WaveformLoaderThread> waveformLoaderThreads;
  std::unique_ptr<IndexLoaderJob> indexLoaderJob;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
