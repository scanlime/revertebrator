#include "GrainData.h"
#include "ZipReader64.h"
#include <deque>
#include <mutex>

juce::Result GrainIndex::load() {
  using juce::var;

  if (!file.existsAsFile()) {
    return juce::Result::fail("No grain data file");
  }
  ZipReader64 zip(file);
  if (!zip.openedOk()) {
    return juce::Result::fail("Can't open file");
  }

  var json;
  {
    auto file = zip.open("index.json");
    if (file != nullptr) {
      json = juce::JSON::parse(*file);
    }
  }
  {
    auto file = zip.open("grains.u64");
    if (file != nullptr) {
      juce::BufferedInputStream buf(*file, 8192);
      while (!buf.isExhausted()) {
        grainX.add(buf.readInt64());
      }
    }
  }
  soundFileBytes = zip.getByteRange("sound.flac");
  if (!json.isObject() || soundFileBytes.isEmpty() || grainX.isEmpty()) {
    return juce::Result::fail("Wrong file format");
  }

  numSamples = json.getProperty("sound_len", var());
  maxGrainWidth = json.getProperty("max_grain_width", var());
  sampleRate = json.getProperty("sample_rate", var());
  auto varBinX = json.getProperty("bin_x", var());
  auto varBinF0 = json.getProperty("bin_f0", var());
  if (varBinX.isArray()) {
    for (auto x : *varBinX.getArray()) {
      binX.add(juce::int64(x));
    }
  }
  if (varBinF0.isArray()) {
    for (auto f0 : *varBinF0.getArray()) {
      binF0.add(f0);
    }
  }

  if (sampleRate < 1 || maxGrainWidthSamples() < 1 || numSamples < 1 ||
      numGrains() < 1 || numBins() < 1 || varBinX.size() != numBins() + 1) {
    return juce::Result::fail("Bad parameters in file");
  }
  return juce::Result::ok();
}

GrainIndex::GrainIndex(const juce::File &file) : file(file), status(load()) {}
GrainIndex::~GrainIndex() {}

juce::String GrainIndex::describeToString() const {
  using juce::String;
  return String(numGrains()) + " grains, " + String(numBins()) + " bins, " +
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
  struct Job {
    GrainIndex::Ptr index;
    GrainWaveform::Key key;
  };

  WaveformLoaderThread() : Thread("grain-waveform") {}

  void addJob(const Job &j) {
    {
      std::lock_guard<std::mutex> guard(workMutex);
      workQueue.push_back(j);
    }
    notify();
  }

  void run() override {
    while (!threadShouldExit()) {
      workMutex.lock();
      if (workQueue.empty()) {
        workMutex.unlock();
        wait(-1);
      } else {
        Job job = workQueue.front();
        workQueue.pop_front();
        workMutex.unlock();
        runJob(job);
      }
    }
  }

private:
  class Stream : public juce::InputStream {
  public:
    // Minimal high performance alternative to juce::SubregionStream

    Stream(const juce::File &file, const juce::Range<juce::int64> &bytes)
        : inner(file), bytes(bytes) {
      setPosition(0);
    }

    ~Stream() override {}
    bool isExhausted() override { return false; }
    juce::int64 getTotalLength() override { return bytes.getLength(); }

    juce::int64 getPosition() override {
      return inner.getPosition() - bytes.getStart();
    }

    bool setPosition(juce::int64 newPosition) override {
      // JUCE's FLAC wrapper has a bug! (unnecessary cast to int)
      jassert(newPosition >= 0);

      return inner.setPosition(newPosition + bytes.getStart());
    }

    int read(void *destBuffer, int maxBytesToRead) {
      return inner.read(destBuffer, maxBytesToRead);
    }

  private:
    juce::FileInputStream inner;
    juce::Range<juce::int64> bytes;
  };

  std::mutex workMutex;
  std::deque<Job> workQueue;

  juce::FlacAudioFormat flac;
  juce::File currentFile;
  std::unique_ptr<juce::AudioFormatReader> reader;

  void runJob(const Job &job) {
    if (!job.index) {
      return;
    }
    GrainIndex &index = *job.index;
    jassert(index.isValid());
    auto grainX = index.grainX[job.key.grain];

    if (currentFile != index.file) {
      currentFile = index.file;
      reader = nullptr;
    }

    if (!reader) {
      auto stream = new Stream(index.file, index.soundFileBytes);
      reader = std::unique_ptr<juce::AudioFormatReader>(
          flac.createReaderFor(stream, true));
    }

    if (reader) {
      GrainWaveform::Ptr newWave = new GrainWaveform(job.key, grainX, *reader);
      index.cacheWaveform(*newWave);
    }
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformLoaderThread)
};

int GrainIndex::Hasher::generateHash(const GrainWaveform::Window &w,
                                     int upperLimit) const noexcept {
  return juce::DefaultHashFunctions::generateHash(
      int(w.mix * 1024.0) ^ (w.width0 * 2) ^ (w.width1 * 3) ^ w.phase1,
      upperLimit);
}

int GrainIndex::Hasher::generateHash(const GrainWaveform::Key &k,
                                     int upperLimit) const noexcept {
  return juce::DefaultHashFunctions::generateHash(
      k.grain ^ int(k.speedRatio * 1e3) ^ generateHash(k.window, upperLimit),
      upperLimit);
}

GrainData::GrainData(juce::ThreadPool &generalPurposeThreads)
    : indexLoaderJob(std::make_unique<IndexLoaderJob>(generalPurposeThreads)) {

  for (auto i = juce::SystemStats::getNumCpus(); i; --i) {
    waveformLoaderThreads.add(new WaveformLoaderThread());
  }
  for (auto t : waveformLoaderThreads) {
    t->startThread();
  }
}

GrainData::~GrainData() {
  for (auto t : waveformLoaderThreads) {
    t->signalThreadShouldExit();
    t->notify();
  }
  for (auto t : waveformLoaderThreads) {
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

GrainWaveform::Ptr GrainData::getWaveform(GrainIndex &index,
                                          const GrainWaveform::Key &key) {
  jassert(index.isValid());
  auto cached = index.getCachedWaveform(key);
  if (cached != nullptr) {
    return cached;
  }

  // Rotate between all threads, only interacting with one thread's work queue
  int seq = (waveformThreadSequence += 1) % waveformLoaderThreads.size();
  waveformLoaderThreads[seq]->addJob(
      WaveformLoaderThread::Job{.index = index, .key = key});
  return nullptr;
}

GrainWaveform::GrainWaveform(const Key &key, juce::uint64 grainX,
                             juce::AudioFormatReader &reader)
    : key(key) {
  auto range = key.window.range();
  auto numChannels = reader.getChannelLayout().size();
  buffer.setSize(numChannels, range.getLength());
  auto writePtr = buffer.getArrayOfWritePointers();

  // 1. Load original FLAC data from the windowed area
  if (!reader.read(writePtr, numChannels, grainX + range.getStart(),
                   range.getLength())) {
    buffer.clear();
    return;
  }

  // 2. Apply window function, while tracking RMS level
  double accum = 0.;
  for (int i = 0; i < range.getLength(); i++) {
    auto y = key.window.evaluate(range.getStart() + i);
    for (int ch = 0; ch < numChannels; ch++) {
      accum += juce::square(writePtr[ch][i] *= y);
    }
  }

  // 3. Level normalization
  // to do: fix units
  // to do: resampling
  // buffer.applyGain(1e3 / std::sqrt(accum * range.getLength() * numChannels));
}

GrainWaveform::~GrainWaveform() {}
