#pragma once

#include <JuceHeader.h>
#include <mutex>

class GrainIndex : public juce::ReferenceCountedObject {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainIndex>;

  GrainIndex(const juce::File &);
  ~GrainIndex() override;

  juce::File file;

  unsigned numGrains{0}, numBins{0}, numChannels{0};
  double sampleRate{0}, maxGrainWidth{0};
  juce::uint64 numSamples{0}, soundByteOffset{0}, soundByteLength{0};

  juce::Array<unsigned> binX;
  juce::Array<float> binF0;
  juce::Array<juce::uint64> grainX;

  juce::Result status;

  inline unsigned closestBinForPitch(float hz) const {
    auto x = std::lower_bound(binF0.begin(), binF0.end(), hz) - binF0.begin();
    return (fabs(binF0[x - 1] - hz) < fabs(binF0[x] - hz)) ? (x - 1) : x;
  }

  inline juce::Range<float> pitchRange() const {
    return juce::Range<float>(binF0[0], binF0[numBins - 1]);
  }

  inline juce::Range<unsigned> grainsForBin(unsigned bin) const {
    return juce::Range<unsigned>(binX[bin], binX[bin + 1]);
  }

  juce::String describeToString() const;
  static juce::String numSamplesToString(juce::uint64 numSamples);

private:
  juce::Result load();

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainIndex)
};

struct GrainWindow {
  float mix;
  int width0, width1, phase1;

  GrainWindow(const GrainIndex &, float mix, float w0, float w1, float p1);
  bool operator==(const GrainWindow &) noexcept;
};

class GrainWaveform : public juce::ReferenceCountedObject {
public:
  using Ptr = juce::ReferenceCountedObjectPtr<GrainWaveform>;

  struct Key {
    unsigned grain;
    float speedRatio;
    GrainWindow window;

    bool operator==(const Key &) noexcept;
  };

  GrainWaveform(const GrainIndex &, const Key &, juce::AudioFormatReader &);
  ~GrainWaveform() override;

  Key key;
  juce::AudioBuffer<float> buffer;

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainWaveform)
};

class GrainData {
public:
  GrainData(juce::ThreadPool &generalPurposeThreads);
  virtual ~GrainData();

  void referFileInputTo(const juce::Value &);
  void referToStatusOutput(juce::Value &);

  GrainIndex::Ptr getIndex();
  GrainWaveform::Ptr getWaveform(const GrainWaveform::Key &);

private:
  struct Hasher {
    int generateHash(const GrainWindow &, int) noexcept;
    int generateHash(const GrainWaveform::Key &, int) noexcept;
  };

  class IndexLoaderJob;
  class WaveformLoaderThread;
  using Key = GrainWaveform::Key;
  using Cache = juce::HashMap<Key, GrainWaveform::Ptr, Hasher>;

  juce::OwnedArray<WaveformLoaderThread> waveformLoaderThreads;
  std::unique_ptr<IndexLoaderJob> indexLoaderJob;

  std::mutex cacheMutex;
  Cache cache;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
