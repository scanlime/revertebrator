#include "GrainData.h"

using juce::int64;
using juce::JSON;
using juce::String;
using juce::uint64;
using juce::var;

GrainData::GrainData()
    : loadingThread("grain_data_loader"), state(std::make_unique<State>()) {
  srcValue.addListener(this);
}

GrainData::~GrainData() { loadingThread.removeTimeSliceClient(this); }

void GrainData::startThread() { loadingThread.startThread(); }

void GrainData::stopThread(int timeOutMilliseconds) {
  loadingThread.stopThread(timeOutMilliseconds);
}

void GrainData::referFileInputTo(const juce::Value &v) {
  juce::ScopedLock sl(valuesMutex);
  srcValue.referTo(v);
}

void GrainData::referToStatusOutput(juce::Value &v) {
  juce::ScopedLock sl(valuesMutex);
  v.referTo(statusValue);
}

GrainData::Accessor::Accessor(GrainData &data)
    : ref(data), reader(data.stateMutex) {}

bool GrainData::Accessor::read(float *const *destChannels, int numDestChannels,
                               int64 startSampleInSource,
                               int numSamplesToRead) {
  auto &reader = ref.state->reader;
  if (reader) {
    return reader->read(destChannels, numDestChannels, startSampleInSource,
                        numSamplesToRead);
  } else {
    return false;
  }
}

String GrainData::Accessor::describeToString() const {
  return String(numGrains()) + " grains, " + String(numBins()) + " bins, " +
         String(maxGrainWidth(), 1) + " sec, " +
         String(pitchRange().getStart(), 1) + " - " +
         String(pitchRange().getEnd(), 1) + " Hz, " + numSamplesToString();
}

String GrainData::Accessor::numSamplesToString() const {
  static const struct {
    const char *prefix;
    double scale;
    int precision;
  } units[] = {
      {"tera", 1e12, 2}, {"giga", 1e9, 2}, {"mega", 1e6, 1},
      {"kilo", 1e3, 1},  {"", 1, 0},
  };

  auto samples = numSamples();
  auto unit = &units[0];
  while (samples < unit->scale && unit->precision > 0) {
    unit++;
  }

  return String(samples / unit->scale, unit->precision) + " " + unit->prefix +
         "samples";
}

void GrainData::valueChanged(juce::Value &) {
  juce::ScopedLock sl(valuesMutex);
  if (srcValue.toString().isNotEmpty()) {
    statusValue.setValue("Loading grain index...");
    loadingThread.addTimeSliceClient(this);
  }
}

int GrainData::useTimeSlice() {
  String srcToLoad;
  {
    juce::ScopedLock sl(valuesMutex);
    srcToLoad = srcValue.toString();
  }
  if (srcToLoad == latestLoadingAttempt) {
    // Idle
    return -1;
  }

  latestLoadingAttempt = srcToLoad;
  String newStatus = load(srcToLoad);
  {
    juce::ScopedLock sl(valuesMutex);
    statusValue.setValue(newStatus);
  }
  // Check again in case a change occurred while we were loading
  return 0;
}

juce::String GrainData::load(const juce::File &srcFile) {
  auto result = std::make_unique<State>();

  if (!srcFile.existsAsFile()) {
    return "No grain data file";
  }
  result->zip = std::make_unique<juce::ZipFile>(srcFile);

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
  result->sampleRate = unsigned(int64(json.getProperty("sample_rate", var())));
  auto varBinX = json.getProperty("bin_x", var());
  auto varBinF0 = json.getProperty("bin_f0", var());

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

  // Grain X table is large so it gets a separate binary file outside the JSON
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

    // Keep the buffer smallish until BufferingAudioReader is more efficient.
    // The balance here now is that the buffer chews CPU if it has many blocks,
    // the block size is not adjustable, and too small a buffer will cause us
    // to read samples we don't actually have space to store when playing many
    // overlapping grains.

    const int64 bufferSize = 0; // 8 * 1024 * 1024;
    if (bufferSize > 0) {
      auto buffer = std::make_unique<juce::BufferingAudioReader>(
          formatReader, loadingThread, bufferSize);
      buffer->setReadTimeout(5);
      result->reader = std::move(buffer);
    } else {
      result->reader = std::unique_ptr<juce::AudioFormatReader>(formatReader);
    }

    // Here is where the new data becomes available to other threads
    {
      juce::ScopedWriteLock writer(stateMutex);
      std::swap(result, state);
    }
    return GrainData::Accessor(*this).describeToString();
  }
}
