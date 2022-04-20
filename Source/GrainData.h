#pragma once

#include <JuceHeader.h>

class GrainData : private juce::Value::Listener {
public:
  GrainData();
  ~GrainData() override;

  juce::Value src, status;

private:
  void valueChanged(juce::Value &) override;
  void reload();

  juce::File srcFile, soundFile;
  std::unique_ptr<juce::AudioFormatReader> reader;
  juce::int64 soundLen;
  float maxGrainWidth;
  int sampleRate;
  juce::Array<int> binX;
  juce::Array<float> binF0;
  juce::Array<juce::int64> grainX;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GrainData)
};
