#include "GrainData.h"
#include "ZipReader64.h"
#include <FLAC/all.h>
#include <deque>
#include <mutex>

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

  ~WaveformLoaderThread() override {
    if (decoder) {
      FLAC__stream_decoder_delete(decoder);
    }
  }

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
  std::mutex workMutex;
  std::deque<Job> workQueue;

  std::unique_ptr<juce::FileInputStream> stream;
  juce::Range<juce::int64> byteRange;
  FLAC__StreamDecoder *decoder{nullptr};

  GrainWaveform::Ptr loadingBuffer;
  juce::int64 firstSampleToLoad{0};
  int loadingProgress;

  void runJob(const Job &job) {
    if (!job.index) {
      return;
    }
    GrainIndex &index = *job.index;
    jassert(index.isValid());

    if (!stream || stream->getFile() != index.file) {
      stream = std::make_unique<juce::FileInputStream>(index.file);
      byteRange = index.soundFileBytes;
      if (decoder) {
        FLAC__stream_decoder_delete(decoder);
        decoder = nullptr;
      }
    }

    if (decoder == nullptr) {
      decoder = FLAC__stream_decoder_new();
      jassert(decoder != nullptr);
      if (decoder == nullptr) {
        return;
      }
      stream->setPosition(byteRange.getStart());
      {
        auto status = FLAC__stream_decoder_init_stream(
            decoder, flacRead, flacSeek, flacTell, flacLength, flacEOF,
            flacWrite, flacMetadata, flacError, this);
        jassert(status == FLAC__STREAM_DECODER_INIT_STATUS_OK);
      }
      {
        auto status =
            FLAC__stream_decoder_process_until_end_of_metadata(decoder);
        jassert(status == true);
      }
    }

    auto range = job.key.window.range();

    // Callbacks append to this buffer as we read compressed blocks
    loadingBuffer = new GrainWaveform(job.key, 1, range.getLength());
    loadingProgress = 0;

    jassert(job.key.grain < index.numGrains());
    firstSampleToLoad = index.grainX[job.key.grain] + range.getStart();
    jassert(firstSampleToLoad >= 0);

    if (!FLAC__stream_decoder_seek_absolute(decoder, firstSampleToLoad)) {
      jassertfalse;
      return;
    };

    while (loadingProgress < range.getLength()) {
      if (!FLAC__stream_decoder_process_single(decoder)) {
        jassertfalse;
        return;
      }
    }

    jassert(loadingBuffer->key == job.key);
    job.key.window.applyToBuffer(loadingBuffer->buffer);
    index.cacheWaveform(*loadingBuffer);
    loadingBuffer = nullptr;
  }

  static FLAC__StreamDecoderSeekStatus
  flacSeek(const FLAC__StreamDecoder *, FLAC__uint64 absolute_byte_offset,
           void *client_data) {
    auto self = static_cast<WaveformLoaderThread *>(client_data);
    self->stream->setPosition(absolute_byte_offset +
                              self->byteRange.getStart());
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
  }

  static FLAC__StreamDecoderTellStatus
  flacTell(const FLAC__StreamDecoder *, FLAC__uint64 *absolute_byte_offset,
           void *client_data) {
    auto self = static_cast<WaveformLoaderThread *>(client_data);
    *absolute_byte_offset =
        self->stream->getPosition() - self->byteRange.getStart();
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
  }

  static FLAC__StreamDecoderLengthStatus flacLength(const FLAC__StreamDecoder *,
                                                    FLAC__uint64 *stream_length,
                                                    void *client_data) {
    auto self = static_cast<WaveformLoaderThread *>(client_data);
    *stream_length = self->byteRange.getLength();
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
  }

  static FLAC__bool flacEOF(const FLAC__StreamDecoder *, void *client_data) {
    auto self = static_cast<WaveformLoaderThread *>(client_data);
    return self->stream->getPosition() >= self->byteRange.getEnd();
  }

  static void flacMetadata(const FLAC__StreamDecoder *,
                           const FLAC__StreamMetadata *, void *) {}

  static void flacError(const FLAC__StreamDecoder *,
                        FLAC__StreamDecoderErrorStatus status, void *) {
    jassertfalse;
  }

  static FLAC__StreamDecoderReadStatus flacRead(const FLAC__StreamDecoder *,
                                                FLAC__byte buffer[],
                                                size_t *bytes,
                                                void *client_data) {
    auto self = static_cast<WaveformLoaderThread *>(client_data);
    *bytes = self->stream->read(buffer, *bytes);
    return (*bytes == 0) ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM
                         : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }

  static FLAC__StreamDecoderWriteStatus
  flacWrite(const FLAC__StreamDecoder *, const FLAC__Frame *frame,
            const FLAC__int32 *const buffer[], void *client_data) {

    auto self = static_cast<WaveformLoaderThread *>(client_data);
    auto blocksize = frame->header.blocksize;
    auto numChannels = self->loadingBuffer->buffer.getNumChannels();
    if (frame->header.channels < numChannels) {
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    if (frame->header.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER) {
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    auto sampleNumber = frame->header.number.sample_number;

    auto progress = self->loadingProgress;
    auto firstSampleToLoad = self->firstSampleToLoad;
    auto outputSize = self->loadingBuffer->buffer.getNumSamples();
    auto output = self->loadingBuffer->buffer.getArrayOfWritePointers();
    jassert(progress <= outputSize);

    if (sampleNumber > firstSampleToLoad + progress) {
      // Past where we need to be
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    auto samplesToSkip = firstSampleToLoad + progress - sampleNumber;
    if (samplesToSkip >= blocksize) {
      // Skipping entire blocks
      return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    int samplesToStore =
        std::min<int>(blocksize - samplesToSkip, outputSize - progress);

    for (auto ch = 0; ch < numChannels; ch++) {
      for (auto i = 0; i < samplesToStore; i++) {
        output[ch][progress + i] = buffer[ch][samplesToSkip + i];
      }
    }
    self->loadingProgress = progress + samplesToStore;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformLoaderThread)
};

GrainWaveform::GrainWaveform(const Key &key, int channels, int samples)
    : key(key), buffer(channels, samples) {}

GrainWaveform::~GrainWaveform() {}

GrainIndex::GrainIndex(const juce::File &file) : file(file), status(load()) {}

GrainIndex::~GrainIndex() {}

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

void GrainIndex::cacheWaveform(GrainWaveform &wave) {
  std::lock_guard<std::mutex> guard(cacheMutex);
  cache.set(wave.key, wave);
}

GrainWaveform::Ptr
GrainIndex::getCachedWaveformOrInsertEmpty(const GrainWaveform::Key &k) {
  std::lock_guard<std::mutex> guard(cacheMutex);
  auto ptr = cache[k];
  if (ptr) {
    return ptr;
  } else {
    GrainWaveform::Ptr empty = new GrainWaveform(k, 0, 0);
    cache.set(k, *empty);
    return nullptr;
  }
}

GrainWaveform::Ptr GrainData::getWaveform(GrainIndex &index,
                                          const GrainWaveform::Key &key) {
  jassert(index.isValid());
  jassert(key.grain < index.numGrains());

  auto cached = index.getCachedWaveformOrInsertEmpty(key);
  if (cached == nullptr) {
    // Totally new item, dispatch it to a rotating worker thread.
    // The cache atomically stored a placeholder to avoid duplicating work.
    int seq = (waveformThreadSequence += 1) % waveformLoaderThreads.size();
    waveformLoaderThreads[seq]->addJob(
        WaveformLoaderThread::Job{.index = index, .key = key});
    return nullptr;
  }
  if (cached->isEmpty()) {
    // It's the placeholder item. Work is still pending.
    return nullptr;
  }
  return cached;
}
