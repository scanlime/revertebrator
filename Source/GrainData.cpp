#include "GrainData.h"

class ZipReader64 {
public:
  ZipReader64(const juce::File &file) : stream(file), fileSize(file.getSize()) {
    readHeaders();
  }
  virtual ~ZipReader64() {}

  juce::InputStream *open(const juce::String &name) {
    auto f = files[name];
    if (f.compressed > 0) {
      auto region =
          new juce::SubregionStream(&stream, f.offset, f.compressed, false);
      if (f.uncompressed != f.compressed) {
        return new juce::GZIPDecompressorInputStream(
            region, true,
            juce::GZIPDecompressorInputStream::Format::deflateFormat,
            f.uncompressed);
      } else {
        return region;
      }
    }
    return nullptr;
  }

private:
  struct FileInfo {
    juce::int64 compressed{0}, uncompressed{0}, offset{0};
  };

  struct EndOfCentralDirectory {
    juce::int64 dirOffset{0}, dirSize{0}, totalDirEntries{0};

    bool read(juce::InputStream &in, juce::int64 pos, juce::int64 fileSize) {
      in.setPosition(pos);

      // Optional 64-bit EOCD locator may appear before the EOCD
      uint32_t eocd64Locator_signature = in.readInt();
      uint32_t eocd64Locator_disk = in.readInt();
      uint64_t eocd64Locator_offset = in.readInt64();
      uint32_t eocd64Locator_totalDisks = in.readInt();

      // Original EOCD
      uint32_t eocd_signature = in.readInt();
      uint16_t eocd_thisDisk = in.readShort();
      uint16_t eocd_dirDisk = in.readShort();
      uint16_t eocd_diskDirEntries = in.readShort();
      uint16_t eocd_totalDirEntries = in.readShort();
      uint32_t eocd_dirSize = in.readInt();
      uint32_t eocd_dirOffset = in.readInt();
      uint16_t eocd_commentLen = in.readShort();

      if (eocd_signature == 0x06054b50 && eocd_thisDisk == 0 &&
          eocd_dirDisk == 0 && eocd_diskDirEntries == eocd_totalDirEntries &&
          pos + readLength + eocd_commentLen <= fileSize) {
        dirOffset = eocd_dirOffset;
        dirSize = eocd_dirSize;
        totalDirEntries = eocd_totalDirEntries;
      } else {
        return false;
      }

      if (eocd64Locator_signature == 0x07064b50 && eocd64Locator_disk == 0 &&
          eocd64Locator_totalDisks == 1 && eocd64Locator_offset < pos) {
        in.setPosition(eocd64Locator_offset);

        // Optional EOCD64
        uint32_t eocd64_signature = in.readInt();
        uint64_t eocd64_size = in.readInt64();
        uint16_t eocd64_versionMadeBy = in.readShort();
        uint16_t eocd64_versionToExtract = in.readShort();
        uint32_t eocd64_thisDisk = in.readInt();
        uint32_t eocd64_dirDisk = in.readInt();
        uint64_t eocd64_diskDirEntries = in.readInt64();
        uint64_t eocd64_totalDirEntries = in.readInt64();
        uint64_t eocd64_dirSize = in.readInt64();
        uint64_t eocd64_dirOffset = in.readInt64();

        if (eocd64_signature == 0x06064b50 && eocd64_thisDisk == 0 &&
            eocd64_dirDisk == 0 &&
            eocd64_diskDirEntries == eocd64_totalDirEntries) {
          // If EOCD64 looks good, it overrides original EOCD
          dirOffset = eocd64_dirOffset;
          dirSize = eocd64_dirSize;
          totalDirEntries = eocd64_totalDirEntries;
        }
      }
      return true;
    }

    bool search(juce::InputStream &in, juce::int64 fileSize) {
      auto first = std::max<juce::int64>(0, fileSize - readLength);
      auto last = std::max<juce::int64>(0, fileSize - maxSpaceAfterward);
      for (auto pos = first; pos >= last; pos--) {
        if (read(in, pos, fileSize)) {
          return true;
        }
      }
      return false;
    }

  private:
    static constexpr int readLength = 42;
    static constexpr int maxSpaceAfterward = 64 * 1024;
  };

  juce::FileInputStream stream;
  juce::int64 fileSize;
  juce::HashMap<juce::String, FileInfo> files;

  void readHeaders() {
    juce::BufferedInputStream buffered(&stream, 8192, false);

    EndOfCentralDirectory eocd;
    if (!eocd.search(buffered, fileSize)) {
      return;
    }

    printf("found central dir, %lld %lld %lld\n", eocd.dirOffset, eocd.dirSize,
           eocd.totalDirEntries);
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZipReader64)
};

GrainIndex::GrainIndex(const juce::File &file)
    : file(file), status(loadIndex()) {}
GrainIndex::~GrainIndex() {}

juce::Result GrainIndex::loadIndex() {
  if (!file.existsAsFile()) {
    return juce::Result::fail("No grain data file");
  }

  printf("what if we loaded a file... %s\n", file.getFullPathName().toUTF8());
  ZipReader64 zip(file);

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

GrainWaveform::Window::Window(float maxWidthSamples, float mix, float w0,
                              float w1, float p1)
    : mix(juce::jlimit(0.f, 1.f, mix)),
      width0(1 +
             std::round(juce::jlimit(0.f, 1.f, w0) * (maxWidthSamples - 1.f))),
      width1(width0 + std::round(juce::jlimit(0.f, 1.f, w0) *
                                 (maxWidthSamples - float(width0)))),
      phase1(std::round(juce::jlimit(-1.f, 1.f, p1) *
                        (maxWidthSamples - float(width1)))) {
  jassert(mix >= 0.f && mix <= 1.f);
  jassert(width0 >= 1 && width0 <= std::ceil(maxWidthSamples));
  jassert(width1 >= width0 && width1 <= std::ceil(maxWidthSamples));
  jassert(std::abs(phase1) <= std::ceil(maxWidthSamples));
}

bool GrainWaveform::Window::operator==(const Window &o) noexcept {
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

int GrainIndex::Hasher::generateHash(const GrainWaveform::Window &w,
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
