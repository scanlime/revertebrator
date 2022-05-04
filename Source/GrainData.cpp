#include "GrainData.h"

class Zip64Reader {
public:
  Zip64Reader(const juce::File &file) : stream(file) {
    printf("%lld\n", file.getSize());
  }

  virtual ~Zip64Reader() {}

private:
  juce::FileInputStream stream;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Zip64Reader)
};

GrainIndex::GrainIndex(const juce::File &file)
    : file(file), status(loadIndex()) {}
GrainIndex::~GrainIndex() {}

juce::Result GrainIndex::loadIndex() {
  if (!file.existsAsFile()) {
    return juce::Result::fail("No grain data file");
  }

  printf("what if we loaded a file... %s\n", file.getFullPathName().toUTF8());
  Zip64Reader zip(file);

  /*
      int entryIndex = result->zip->getIndexOfFileName("index.json");
      int entryGrains = result->zip->getIndexOfFileName("grains.u64");
      int entrySound = result->zip->getIndexOfFileName("sound.flac");
      if (entryIndex < 0 || entryGrains < 0 || entrySound < 0) {
        return "Wrong file format, failed to understand archive";
      }

      var json;
      {
        auto stream = std::unique_ptr<juce::InputStream>(
            result->zip->createStreamForEntry(entryIndex));
        if (stream) {
          json = JSON::parse(*stream);
        }
      }
      if (!json.isObject()) {
        return "Failed to load JSON index within grain archive";
      }
      result->soundLen = uint64(int64(json.getProperty("sound_len", var())));
      result->maxGrainWidth = json.getProperty("max_grain_width", var());
      result->sampleRate = unsigned(int64(json.getProperty("sample_rate",
     var()))); auto varBinX = json.getProperty("bin_x", var()); auto varBinF0 =
     json.getProperty("bin_f0", var());

      // Bin index (int list)
      if (varBinX.isArray()) {
        for (auto x : *varBinX.getArray()) {
          result->binX.add(unsigned(int64(x)));
        }
      }

      // Bin fundamental frequencies (float list)
      if (varBinF0.isArray()) {
        for (auto f0 : *varBinF0.getArray()) {
          result->binF0.add(f0);
        }
      }

      // Grain X table is large so it gets a separate binary file outside the
     JSON
      {
        auto stream = std::unique_ptr<juce::InputStream>(
            result->zip->createStreamForEntry(entryGrains));
        if (stream) {
          juce::BufferedInputStream buffer(*stream, 64 * 1024);
          while (!buffer.isExhausted()) {
            result->grainX.add(uint64(buffer.readInt64()));
          }
        }

        juce::FlacAudioFormat flac;
        auto formatReader = flac.createReaderFor(
            result->zip->createStreamForEntry(entrySound), true);
        if (!formatReader) {
          return "Can't read from sound file";
        }
  */
  return juce::Result::ok();
}

juce::String GrainIndex::describeToString() const {
  using juce::String;
  return String(numGrains) + " grains, " + String(numBins) + " bins, " +
         String(maxGrainWidth, 1) + " sec, " +
         String(pitchRange().getStart(), 1) + " - " +
         String(pitchRange().getEnd(), 1) + " Hz, " +
         numSamplesToString(numSamples);
}

juce::String GrainIndex::numSamplesToString(juce::uint64 samples) {
  static const struct {
    const char *prefix;
    double scale;
    int precision;
  } units[] = {
      {"tera", 1e12, 2}, {"giga", 1e9, 2}, {"mega", 1e6, 1},
      {"kilo", 1e3, 1},  {"", 1, 0},
  };
  auto unit = &units[0];
  while (samples < unit->scale && unit->precision > 0) {
    unit++;
  }
  return juce::String(samples / unit->scale, unit->precision) + " " +
         unit->prefix + "samples";
}

GrainWaveform::Ptr GrainIndex::getWaveform(const GrainWaveform::Key &) {
  return nullptr;
}

class GrainData::IndexLoaderJob : private juce::ThreadPoolJob,
                                  private juce::Value::Listener {
public:
  IndexLoaderJob(juce::ThreadPool &pool)
      : ThreadPoolJob("grain-index"), pool(pool) {
    srcValue.addListener(this);
  }

  ~IndexLoaderJob() override { pool.waitForJobToFinish(this, -1); }

  void referFileInputTo(const juce::Value &v) {
    juce::ScopedLock guard(valuesRecursiveMutex);
    srcValue.referTo(v);
  }

  void referToStatusOutput(juce::Value &v) {
    juce::ScopedLock guard(valuesRecursiveMutex);
    v.referTo(statusValue);
  }

  GrainIndex::Ptr getIndex() {
    std::lock_guard<std::mutex> guard(indexMutex);
    return indexPtr;
  }

private:
  void valueChanged(juce::Value &) {
    juce::ScopedLock guard(valuesRecursiveMutex);
    if (srcValue.toString().isNotEmpty()) {
      statusValue.setValue("Loading grain index...");
      if (!isPending) {
        isPending = true;
        pool.addJob(this, false);
      }
    }
  }

  JobStatus runJob() override {
    juce::String srcToLoad;
    {
      juce::ScopedLock guard(valuesRecursiveMutex);
      srcToLoad = srcValue.toString();
      if (srcToLoad == latestLoadingAttempt) {
        isPending = false;
        return JobStatus::jobHasFinished;
      }
      latestLoadingAttempt = srcToLoad;
    }
    GrainIndex::Ptr newIndex = new GrainIndex(srcToLoad);
    juce::String newStatus = newIndex->status.wasOk()
                                 ? newIndex->describeToString()
                                 : newIndex->status.getErrorMessage();
    {
      std::lock_guard<std::mutex> guard(indexMutex);
      std::swap(indexPtr, newIndex);
    }
    {
      juce::ScopedLock guard(valuesRecursiveMutex);
      statusValue.setValue(newStatus);
    }
    // Check again in case a change occurred while we were loading
    return JobStatus::jobNeedsRunningAgain;
  }

  juce::ThreadPool &pool;

  std::mutex indexMutex;
  GrainIndex::Ptr indexPtr;

  juce::CriticalSection valuesRecursiveMutex;
  juce::Value srcValue, statusValue;
  bool isPending{false};
  juce::String latestLoadingAttempt;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IndexLoaderJob)
};

class GrainData::WaveformLoaderThread : public juce::Thread {
public:
  WaveformLoaderThread() : Thread("grain-waveform") {}

  void run() override {
    while (!threadShouldExit()) {
      printf("Bored waveform loader thread says hi\n");
      wait(-1);
    }
  }

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformLoaderThread)
};

GrainWindow::GrainWindow(float maxGrainWidthSamples, float mix, float w0,
                         float w1, float p1)
    : mix(juce::jlimit(0.f, 1.f, mix)),
      width0(1 + std::round(juce::jlimit(0.f, 1.f, w0) *
                            (maxGrainWidthSamples - 1.f))),
      width1(width0 + std::round(juce::jlimit(0.f, 1.f, w0) *
                                 (maxGrainWidthSamples - float(width0)))),
      phase1(std::round(juce::jlimit(-1.f, 1.f, p1) *
                        (maxGrainWidthSamples - float(width1)))) {
  jassert(mix >= 0.f && mix <= 1.f);
  jassert(width0 >= 1 && width0 <= std::ceil(maxGrainWidthSamples));
  jassert(width1 >= width0 && width1 <= std::ceil(maxGrainWidthSamples));
  jassert(std::abs(phase1) <= std::ceil(maxGrainWidthSamples));
}

bool GrainWindow::operator==(const GrainWindow &o) noexcept {
  return mix == o.mix && width0 == o.width0 && width1 == o.width1 &&
         phase1 == o.phase1;
}

bool GrainWaveform::Key::operator==(const Key &o) noexcept {
  return grain == o.grain && speedRatio == o.speedRatio && window == o.window;
}

GrainWaveform::GrainWaveform(const Key &key, juce::uint64 grainX,
                             juce::AudioFormatReader &reader)
    : key(key) {}

GrainWaveform::~GrainWaveform() {}

int GrainIndex::Hasher::generateHash(const GrainWindow &w,
                                     int upperLimit) noexcept {
  return juce::DefaultHashFunctions::generateHash(
      int(w.mix * 1024.0) ^ (w.width0 * 2) ^ (w.width1 * 3) ^ w.phase1,
      upperLimit);
}

int GrainIndex::Hasher::generateHash(const GrainWaveform::Key &k,
                                     int upperLimit) noexcept {
  return juce::DefaultHashFunctions::generateHash(
      k.grain ^ int(k.speedRatio * 1e3) ^ generateHash(k.window, upperLimit),
      upperLimit);
}

GrainData::GrainData(juce::ThreadPool &generalPurposeThreads)
    : indexLoaderJob(std::make_unique<IndexLoaderJob>(generalPurposeThreads)) {
  for (auto i = juce::SystemStats::getNumCpus(); i; --i) {
    waveformLoaderThreads.add(new WaveformLoaderThread());
  }
  for (auto *t : waveformLoaderThreads) {
    t->startThread();
  }
}

GrainData::~GrainData() {
  for (auto *t : waveformLoaderThreads) {
    t->signalThreadShouldExit();
    t->notify();
  }
  for (auto *t : waveformLoaderThreads) {
    t->waitForThreadToExit(-1);
  }
}

void GrainData::referFileInputTo(const juce::Value &v) {
  indexLoaderJob->referFileInputTo(v);
}

void GrainData::referToStatusOutput(juce::Value &v) {
  indexLoaderJob->referToStatusOutput(v);
}

GrainIndex::Ptr GrainData::getIndex() { return indexLoaderJob->getIndex(); }
