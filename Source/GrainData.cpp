#include "GrainData.h"

using juce::DefaultHashFunctions;
using juce::Result;
using juce::String;
using juce::ThreadPool;
using juce::ThreadPoolJob;
using juce::Value;

class GrainData::IndexLoaderJob : private ThreadPoolJob,
                                  private Value::Listener {
public:
  IndexLoaderJob(ThreadPool &pool) : ThreadPoolJob("grain-index"), pool(pool) {
    srcValue.addListener(this);
  }

  ~IndexLoaderJob() override { pool.waitForJobToFinish(this, -1); }

  void referFileInputTo(const Value &v) {
    juce::ScopedLock guard(valuesMutex);
    srcValue.referTo(v);
  }

  void referToStatusOutput(Value &v) {
    juce::ScopedLock guard(valuesMutex);
    v.referTo(statusValue);
  }

  GrainIndex::Ptr getIndex() {
    std::lock_guard<std::mutex> guard(indexMutex);
    return indexPtr;
  }

private:
  void valueChanged(Value &) {
    juce::ScopedLock guard(valuesMutex);
    if (srcValue.toString().isNotEmpty()) {
      statusValue.setValue("Loading grain index...");
      if (!isPending) {
        isPending = true;
        pool.addJob(this, false);
      }
    }
  }

  JobStatus runJob() override {
    String srcToLoad;
    {
      juce::ScopedLock guard(valuesMutex);
      srcToLoad = srcValue.toString();
      if (srcToLoad == latestLoadingAttempt) {
        isPending = false;
        return JobStatus::jobHasFinished;
      }
      latestLoadingAttempt = srcToLoad;
    }
    GrainIndex::Ptr newIndex = new GrainIndex(srcToLoad);
    String newStatus = newIndex->status.wasOk()
                           ? newIndex->describeToString()
                           : newIndex->status.getErrorMessage();
    {
      std::lock_guard<std::mutex> guard(indexMutex);
      std::swap(indexPtr, newIndex);
    }
    {
      juce::ScopedLock guard(valuesMutex);
      statusValue.setValue(newStatus);
    }
    // Check again in case a change occurred while we were loading
    return JobStatus::jobNeedsRunningAgain;
  }

  ThreadPool &pool;

  // Nonrecursive mutex for index ptr
  std::mutex indexMutex;
  GrainIndex::Ptr indexPtr;

  // Recursive mutex for input/output Values
  juce::CriticalSection valuesMutex;
  Value srcValue, statusValue;
  bool isPending{false};
  String latestLoadingAttempt;
};

class GrainData::WaveformLoaderThread : public juce::Thread {
public:
  WaveformLoaderThread() : Thread("grain-waveform") {}

  void run() override {}
};

GrainIndex::GrainIndex(const juce::File &file) : file(file), status(load()) {}

GrainIndex::~GrainIndex() {}

Result GrainIndex::load() {
  if (!file.existsAsFile()) {
    return Result::fail("No grain data file");
  }

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
  return Result::ok();
}

String GrainIndex::describeToString() const {
  auto pr = pitchRange();
  return String(numGrains) + " grains, " + String(numBins) + " bins, " +
         String(maxGrainWidth, 1) + " sec, " + String(pr.getStart(), 1) +
         " - " + String(pr.getEnd(), 1) + " Hz, " +
         numSamplesToString(numSamples);
}

String GrainIndex::numSamplesToString(juce::uint64 samples) {
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
  return String(samples / unit->scale, unit->precision) + " " + unit->prefix +
         "samples";
}

GrainWindow::GrainWindow(const GrainIndex &, float mix, float w0, float w1,
                         float p1)
    : mix(mix), width0(0), width1(0), phase1(0) {}

bool GrainWindow::operator==(const GrainWindow &o) noexcept {
  return mix == o.mix && width0 == o.width0 && width1 == o.width1 &&
         phase1 == o.phase1;
}

bool GrainWaveform::Key::operator==(const Key &o) noexcept {
  return grain == o.grain && speedRatio == o.speedRatio && window == o.window;
}

GrainWaveform::GrainWaveform(const GrainIndex &grainIndex, const Key &key,
                             juce::AudioFormatReader &reader)
    : key(key) {}

GrainWaveform::~GrainWaveform() {}

int GrainData::Hasher::generateHash(const GrainWindow &w,
                                    int upperLimit) noexcept {
  return DefaultHashFunctions::generateHash(
      int(w.mix * 1024.0) ^ (w.width0 * 2) ^ (w.width1 * 3) ^ w.phase1,
      upperLimit);
}

int GrainData::Hasher::generateHash(const GrainWaveform::Key &k,
                                    int upperLimit) noexcept {
  return DefaultHashFunctions::generateHash(
      k.grain ^ int(k.speedRatio * 1e3) ^ generateHash(k.window, upperLimit),
      upperLimit);
}

GrainData::GrainData(juce::ThreadPool &generalPurposeThreads)
    : indexLoaderJob(std::make_unique<IndexLoaderJob>(generalPurposeThreads)) {}

GrainData::~GrainData() {}

void GrainData::referFileInputTo(const Value &v) {
  indexLoaderJob->referFileInputTo(v);
}

void GrainData::referToStatusOutput(Value &v) {
  indexLoaderJob->referToStatusOutput(v);
}

GrainIndex::Ptr GrainData::getIndex() { return indexLoaderJob->getIndex(); }

GrainWaveform::Ptr GrainData::getWaveform(const GrainWaveform::Key &) {
  return nullptr;
}
