#include "GrainData.h"

using juce::int64;
using juce::JSON;
using juce::String;
using juce::var;

GrainData::GrainData() {
  src.addListener(this);
  valueChanged(src);
}

GrainData::~GrainData() {}

void GrainData::valueChanged(juce::Value &) {
  auto newSrc = src.toString();
  if (newSrc != srcFile.getFullPathName()) {
    srcFile = newSrc;
    reload();
  }
}

void GrainData::reload() {
  if (!srcFile.existsAsFile()) {
    status.setValue("No grain data file");
    return;
  }

  auto json = JSON::parse(srcFile);
  if (!json.isObject()) {
    status.setValue("Failed to load JSON data!");
    return;
  }

  soundFile =
      srcFile.getSiblingFile(json.getProperty("sound_file", var()).toString());
  if (!soundFile.existsAsFile()) {
    status.setValue("Can't find sound file: " + soundFile.getFullPathName());
    return;
  }

  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
  reader = std::unique_ptr<juce::AudioFormatReader>(
      formats.createReaderFor(soundFile));
  if (!reader) {
    status.setValue("Can't read from sound file");
    return;
  }

  soundLen = json.getProperty("sound_len", var());
  maxGrainWidth = json.getProperty("max_grain_width", var());
  sampleRate = json.getProperty("sample_rate", var());
  auto varBinX = json.getProperty("bin_x", var());
  auto varBinF0 = json.getProperty("bin_f0", var());
  auto varGrainX = json.getProperty("grain_x", var());

  // Bin index (int list)
  binX.clear();
  if (varBinX.isArray()) {
    for (auto x : *varBinX.getArray()) {
      binX.add(x);
    }
  }

  // Bin fundamental frequencies (float list)
  binF0.clear();
  if (varBinF0.isArray()) {
    for (auto f0 : *varBinF0.getArray()) {
      binF0.add(f0);
    }
  }

  // Grain index (int64le array in a base64 string)
  grainX.clear();
  juce::MemoryOutputStream memGrainX;
  if (juce::Base64::convertFromBase64(memGrainX, varGrainX.toString())) {
    juce::MemoryInputStream in(memGrainX.getMemoryBlock());
    while (!in.isExhausted()) {
      grainX.add(in.readInt64());
    }
  }

  status.setValue(String(sampleRate) + " Hz, " + String(soundLen / 1e6) +
                  " Msamples, width " + String(maxGrainWidth) + " sec, " +
                  String(binX.size()) + " bins, " + String(grainX.size()) +
                  " grains");
}
