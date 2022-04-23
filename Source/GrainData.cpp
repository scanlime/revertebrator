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
    : ref(data), reader(data.rwLock) {}

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
  juce::ScopedReadLock reader(rwLock);
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

  // Keep the buffer smallish until BufferingAudioReader is more efficient
  const juce::int64 bufferSize = 32 * 1024 * 1024;
  newState->reader = std::make_unique<juce::BufferingAudioReader>(
      formatReader, loadingThread, bufferSize);

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
    juce::ScopedReadLock writer(rwLock);
    std::swap(newState, state);
  }
  status.setValue(newStatus);
}

String GrainData::State::toString() const {
  return String(sampleRate) + " Hz, " + String(soundLen / 1e6) +
         " Msamples, width " + String(maxGrainWidth) + " sec, " +
         String(numBins()) + " bins, " + String(numGrains()) + " grains";
}
