#include "GrainData.h"

using juce::int64;
using juce::JSON;
using juce::String;
using juce::var;

GrainData::GrainData()
    : loadingThread("grain_data_loader"), state(std::make_unique<State>()) {
  src.addListener(this);
  valueChanged(src);
}

GrainData::~GrainData() {}

void GrainData::startThread() { loadingThread.startThread(); }

GrainData::Accessor::Accessor(GrainData &data)
    : ref(data), reader(data.stateMutex) {}

bool GrainData::Accessor::read(float *const *destChannels, int numDestChannels,
                               juce::int64 startSampleInSource,
                               int numSamplesToRead) {
  auto &reader = ref.state->reader;
  if (reader) {
    return reader->read(destChannels, numDestChannels, startSampleInSource,
                        numSamplesToRead);
  } else {
    return false;
  }
}

int GrainData::Accessor::closestBinForPitch(float hz) const {
  auto begin = ref.state->binF0.begin(), end = ref.state->binF0.end();
  int bin1 = std::lower_bound(begin, end, hz) - begin;
  int bin0 = bin1 - 1;
  float dist0 = fabs(pitchForBin(bin0) - hz);
  float dist1 = fabs(pitchForBin(bin1) - hz);
  return (dist0 < dist1) ? bin0 : bin1;
}

void GrainData::valueChanged(juce::Value &) {
  juce::ScopedReadLock reader(stateMutex);
  auto newSrc = src.toString();
  if (!state || newSrc != state->srcFile.getFullPathName()) {
    load(newSrc);
  }
}

void GrainData::load(juce::String &srcFile) {
  auto newState = std::make_unique<State>();
  newState->srcFile = srcFile;

  if (!newState->srcFile.existsAsFile()) {
    status.setValue("No grain data file");
    return;
  }

  auto json = JSON::parse(newState->srcFile);
  if (!json.isObject()) {
    status.setValue("Failed to load JSON data!");
    return;
  }

  newState->soundFile = newState->srcFile.getSiblingFile(
      json.getProperty("sound_file", var()).toString());
  if (!newState->soundFile.existsAsFile()) {
    status.setValue("Can't find sound file: " +
                    newState->soundFile.getFullPathName());
    return;
  }

  newState->soundLen = json.getProperty("sound_len", var());
  newState->maxGrainWidth = json.getProperty("max_grain_width", var());
  newState->sampleRate = json.getProperty("sample_rate", var());
  auto varBinX = json.getProperty("bin_x", var());
  auto varBinF0 = json.getProperty("bin_f0", var());
  auto varGrainX = json.getProperty("grain_x", var());

  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
  auto formatReader = formats.createReaderFor(newState->soundFile);
  if (!formatReader) {
    status.setValue("Can't read from sound file");
    return;
  }

  // Keep the buffer smallish until BufferingAudioReader is more efficient.
  // The balance here now is that the buffer chews CPU if it has many blocks,
  // the block size is not adjustable, and too small a buffer will cause us
  // to read samples we don't actually have space to store when playing many
  // overlapping grains.

  const juce::int64 bufferSize = 0; // 8 * 1024 * 1024;
  if (bufferSize > 0) {
    auto buffer = std::make_unique<juce::BufferingAudioReader>(
        formatReader, loadingThread, bufferSize);
    buffer->setReadTimeout(5);
    newState->reader = std::move(buffer);
  } else {
    newState->reader = std::unique_ptr<juce::AudioFormatReader>(formatReader);
  }

  // Bin index (int list)
  if (varBinX.isArray()) {
    for (auto x : *varBinX.getArray()) {
      newState->binX.add(x);
    }
  }

  // Bin fundamental frequencies (float list)
  if (varBinF0.isArray()) {
    for (auto f0 : *varBinF0.getArray()) {
      newState->binF0.add(f0);
    }
  }

  // Grain index (int64le array in a base64 string)
  juce::MemoryOutputStream memGrainX;
  if (juce::Base64::convertFromBase64(memGrainX, varGrainX.toString())) {
    juce::MemoryInputStream in(memGrainX.getMemoryBlock());
    while (!in.isExhausted()) {
      newState->grainX.add(in.readInt64());
    }
  }

  auto newStatus = newState->toString();

  {
    juce::ScopedReadLock writer(stateMutex);
    std::swap(newState, state);
  }
  status.setValue(newStatus);
}

String GrainData::State::toString() const {
  String result;
  result += String(numGrains()) + " grains, ";
  result += String(maxGrainWidth, 1) + " sec, ";
  if (numBins() > 0) {
    result +=
        String(binF0[0], 1) + " - " + String(binF0[numBins() - 1], 1) + " Hz, ";
  }
  if (soundLen > 1e12) {
    result += String(soundLen / 1e12, 2) + " terasamples";
  } else if (soundLen > 1e9) {
    result += String(soundLen / 1e9, 2) + " gigasamples";
  } else if (soundLen > 1e6) {
    result += String(soundLen / 1e6, 1) + " megasamples";
  } else if (soundLen > 1e3) {
    result += String(soundLen / 1e3, 1) + " kilosamples";
  } else if (soundLen > 1) {
    result += String(soundLen) + " samples";
  }
  return result;
}
