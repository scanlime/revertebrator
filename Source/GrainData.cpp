#include "GrainData.h"
#include "ZipReader64.h"
#include <FLAC/all.h>
#include <deque>
#include <mutex>

class GrainData::CacheCleanupJob : private juce::ThreadPoolJob,
                                   private juce::Timer {
  static constexpr int intervalMilliseconds = 1500;
  static constexpr int inactivityThreshold = 5;

public:
  CacheCleanupJob(juce::ThreadPool &pool, GrainData &grainData)
      : ThreadPoolJob("cache-cleanup"), pool(pool), grainData(grainData) {
    startTimer(intervalMilliseconds);
  }

  ~CacheCleanupJob() override { pool.waitForJobToFinish(this, -1); }

private:
  juce::ThreadPool &pool;
  GrainData &grainData;
  bool isPending{false};

  void timerCallback() override {
    if (!isPending) {
      isPending = true;
      pool.addJob(this, false);
    }
  }

  JobStatus runJob() override {
    auto index = grainData.getIndex();
    if (index != nullptr) {
      index->cache.cleanup(inactivityThreshold);
    }
    isPending = false;
    return JobStatus::jobHasFinished;
  }
};

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
      // The work queue is actually set up as LIFO so we
      // prioritize new jobs even if there is a backlog.
      std::lock_guard<std::mutex> guard(workMutex);
      workQueue.push_front(j);
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

  int loadQueueDepth() {
    std::lock_guard<std::mutex> guard(workMutex);
    return workQueue.size();
  }

private:
  std::mutex workMutex;
  std::deque<Job> workQueue;

  std::unique_ptr<juce::FileInputStream> stream;
  juce::Range<juce::int64> byteRange;
  FLAC__StreamDecoder *decoder{nullptr};
  juce::Interpolators::WindowedSinc interpolator;

  struct {
    juce::AudioBuffer<float> audio;
    juce::int64 firstSample;
    int progress, size;
  } buffer;

  void runJob(const Job &job) {
    if (!job.index) {
      return;
    }
    GrainIndex &index = *job.index;
    jassert(index.isValid());

    if (!index.cache.contains(job.key)) {
      // Abandon loading items that have already been deleted from the cache
      return;
    }

    // (Re)open the input file as necessary
    if (!stream || stream->getFile() != index.file) {
      stream = std::make_unique<juce::FileInputStream>(index.file);
      byteRange = index.soundFileBytes;
      if (decoder) {
        FLAC__stream_decoder_delete(decoder);
        decoder = nullptr;
      }
    }

    // (Re)init the FLAC decoder as necessary
    if (decoder == nullptr) {
      decoder = FLAC__stream_decoder_new();
      jassert(decoder != nullptr);
      if (decoder == nullptr) {
        return;
      }
      stream->setPosition(byteRange.getStart());
      auto status = FLAC__stream_decoder_init_stream(
          decoder, flacRead, flacSeek, flacTell, flacLength, flacEOF, flacWrite,
          flacMetadata, flacError, this);
      jassert(status == FLAC__STREAM_DECODER_INIT_STATUS_OK);
    }

    jassert(job.key.grain < index.numGrains());
    juce::int64 grainX = index.grainX[job.key.grain];
    auto speedRatio = job.key.speedRatio;
    auto range = job.key.window.range();

    auto resamplerLatency = interpolator.getBaseLatency() / speedRatio;
    juce::Range<int> resampledRange(
        std::floor(range.getStart() * speedRatio - resamplerLatency),
        std::ceil(range.getEnd() * speedRatio));

    buffer.firstSample = resampledRange.getStart() + grainX;
    buffer.size = resampledRange.getLength();
    buffer.progress = 0;

    // Read in FLAC frames containing the audio we want
    if (!FLAC__stream_decoder_seek_absolute(
            decoder, std::max<juce::int64>(0, buffer.firstSample))) {
      jassertfalse;
      return;
    };
    while (buffer.progress < buffer.size) {
      if (!FLAC__stream_decoder_process_single(decoder)) {
        jassertfalse;
        return;
      }
    }
    jassert(buffer.progress == buffer.size);
    jassert(buffer.audio.getNumSamples() == buffer.size);

    auto numChannels = buffer.audio.getNumChannels();
    GrainWaveform::Ptr wave =
        new GrainWaveform(job.key, numChannels, range.getLength());
    auto readPtrs = buffer.audio.getArrayOfReadPointers();
    auto writePtrs = wave->buffer.getArrayOfWritePointers();

    // Resample audio from temp buffer into GrainWaveform buffer
    for (auto ch = 0; ch < numChannels; ch++) {
      interpolator.reset();
      interpolator.process(speedRatio, readPtrs[ch], writePtrs[ch],
                           range.getLength());
    }

    // Apply windowing and RMS normalization in-place in GrainWaveform buffer
    double accum = 0.;
    for (int i = 0; i < range.getLength(); i++) {
      auto window = job.key.window.evaluate(range.getStart() + i);
      for (int ch = 0; ch < numChannels; ch++) {
        accum += double(juce::square<float>(writePtrs[ch][i] *= window));
      }
    }
    auto rms = std::sqrt(accum / double(numChannels * range.getLength()));
    wave->buffer.applyGain(1.0 / rms);

    index.cache.store(*wave);
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

    juce::int64 sampleNumber = frame->header.number.sample_number;
    if (frame->header.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER) {
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    auto self = static_cast<WaveformLoaderThread *>(client_data);
    self->buffer.audio.setSize(frame->header.channels, self->buffer.size);

    auto progress = self->buffer.progress;
    auto remaining = self->buffer.size - progress;
    juce::int64 wantSampleNumber = self->buffer.firstSample + progress;

    auto samplesToZeroFill = std::max<int>(
        0, std::min<int>(sampleNumber - wantSampleNumber, remaining));
    auto samplesToSkip =
        std::max<int>(0, std::min<int>(wantSampleNumber - sampleNumber,
                                       remaining - samplesToZeroFill));
    auto samplesToStore = std::max<int>(
        0, std::min<int>(frame->header.blocksize - samplesToSkip,
                         remaining - samplesToZeroFill - samplesToSkip));

    auto output = self->buffer.audio.getArrayOfWritePointers();
    for (auto ch = 0; ch < frame->header.channels; ch++) {
      for (auto i = 0; i < samplesToZeroFill; i++) {
        output[ch][progress + i] = 0.f;
      }
      for (auto i = 0; i < samplesToStore; i++) {
        output[ch][progress + samplesToZeroFill + i] =
            buffer[ch][samplesToSkip + i];
      }
    }
    self->buffer.progress = progress + samplesToZeroFill + samplesToStore;
    jassert(self->buffer.progress <= self->buffer.size);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformLoaderThread)
};

GrainWaveform::GrainWaveform(const Key &key, int channels, int samples)
    : key(key), buffer(channels, samples) {}

GrainWaveform::~GrainWaveform() {}

GrainIndex::GrainIndex(const juce::File &file) : file(file), status(load()) {}
GrainIndex::~GrainIndex() {}

void GrainWaveformCache::addListener(Listener *listener) {
  std::lock_guard<std::mutex> guard(listenerMutex);
  listeners.add(listener);
}

void GrainWaveformCache::removeListener(Listener *listener) {
  std::lock_guard<std::mutex> guard(listenerMutex);
  listeners.remove(listener);
}

juce::int64 GrainWaveformCache::sizeInBytes() {
  std::lock_guard<std::mutex> guard(cacheMutex);
  return totalBytes;
}

std::size_t
GrainWaveformCache::Hasher::operator()(GrainWaveform::Key const &key) const {
  return key.grain ^ int(key.speedRatio * 1e3) ^ int(key.window.mix * 3e3) ^
         (key.window.width0 * 2) ^ (key.window.width1 * 3) ^ key.window.phase1;
}

void GrainWaveformCache::cleanup(int inactivityThreshold) {
  // Let waveform deletion happen without the cache lock held
  std::vector<GrainWaveform::Ptr> wavesToRelease;
  std::vector<GrainWaveform::Key> keysToRemove;
  {
    std::lock_guard<std::mutex> guard(cacheMutex);

    int sizeOfWavesToRelease = 0;
    int counter = cleanupCounter;
    cleanupCounter = counter + 1;

    for (auto &item : map) {
      int age = counter - item.second.cleanupCounter;
      if (age >= inactivityThreshold) {
        auto &wave = item.second.wave;
        auto waveRefs = wave == nullptr ? 0 : wave->getReferenceCount();
        if (waveRefs <= 1) {
          keysToRemove.push_back(item.first);
          if (wave != nullptr) {
            wavesToRelease.push_back(wave);
            sizeOfWavesToRelease += wave->sizeInBytes();
          }
        }
      }
    }
    for (auto &key : keysToRemove) {
      map.erase(key);
    }
    totalBytes -= sizeOfWavesToRelease;
  }
  {
    std::lock_guard<std::mutex> guard(listenerMutex);
    for (auto &key : keysToRemove) {
      listeners.call([key](Listener &l) { l.grainWaveformExpired(key); });
    }
  }
}

bool GrainWaveformCache::contains(const GrainWaveform::Key &key) {
  std::lock_guard<std::mutex> guard(cacheMutex);
  return map.find(key) != map.end();
}

void GrainWaveformCache::store(GrainWaveform &wave) {
  {
    std::lock_guard<std::mutex> guard(cacheMutex);
    auto &slot = map[wave.key];
    auto prevSize = slot.wave == nullptr ? 0 : slot.wave->sizeInBytes();
    totalBytes += wave.sizeInBytes() - prevSize;
    slot.wave = wave;
    slot.cleanupCounter = cleanupCounter;
  }
  {
    auto key = wave.key;
    std::lock_guard<std::mutex> guard(listenerMutex);
    listeners.call([key](Listener &l) { l.grainWaveformStored(key); });
  }
}

GrainWaveform::Ptr
GrainWaveformCache::lookupOrInsertEmpty(const GrainWaveform::Key &key) {
  GrainWaveform::Ptr result;
  bool dataFound;
  {
    std::lock_guard<std::mutex> guard(cacheMutex);
    auto &slot = map[key];
    slot.cleanupCounter = cleanupCounter;
    if (slot.wave == nullptr) {
      // First time visiting this slot: insert a marker and return null
      slot.wave = new GrainWaveform(key, 0, 0);
      result = nullptr;
      dataFound = false;
    } else {
      // Coming back to a slot we've seen but it may or may not have actual
      // data.
      result = slot.wave;
      dataFound = result->buffer.getNumSamples() > 0;
    }
  }
  {
    std::lock_guard<std::mutex> guard(listenerMutex);
    listeners.call([key, dataFound](Listener &l) {
      l.grainWaveformLookup(key, dataFound);
    });
  }
  return result;
}

static juce::String numSamplesToString(juce::uint64 samples) {
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

juce::String GrainIndex::describeToString() const {
  using juce::String;
  return String(numGrains()) + " grains, " + String(numBins()) + " bins, " +
         String(maxGrainWidth, 1) + " sec, " +
         String(pitchRange().getStart(), 1) + " - " +
         String(pitchRange().getEnd(), 1) + " Hz, " +
         numSamplesToString(numSamples);
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
    : indexLoaderJob(std::make_unique<IndexLoaderJob>(generalPurposeThreads)),
      cacheCleanupJob(
          std::make_unique<CacheCleanupJob>(generalPurposeThreads, *this)) {
  for (auto i = juce::SystemStats::getNumCpus(); i; --i) {
    waveformLoaderThreads.add(new WaveformLoaderThread());
  }
  for (auto t : waveformLoaderThreads) {
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

GrainWaveform::Ptr GrainData::getWaveform(GrainIndex &index,
                                          const GrainWaveform::Key &key) {
  jassert(index.isValid());
  jassert(key.grain < index.numGrains());

  auto cached = index.cache.lookupOrInsertEmpty(key);
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

float GrainData::averageLoadQueueDepth() {
  float totalDepth = 0., totalThreads = 0.;
  for (auto *t : waveformLoaderThreads) {
    totalDepth += t->loadQueueDepth();
    totalThreads += 1.f;
  }
  if (totalThreads == 0.f) {
    jassertfalse;
    return 0.f;
  } else {
    return totalDepth / totalThreads;
  }
}
