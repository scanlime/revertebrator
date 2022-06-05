#pragma once

#include "RvvProcessor.h"
#include <JuceHeader.h>

class RvvEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
  RvvEditor(RvvProcessor &);
  ~RvvEditor() override;

  void paint(juce::Graphics &) override;
  void resized() override;

  static constexpr int defaultWidth = 800;
  static constexpr int defaultHeight = 700;

private:
  struct Parts;

  juce::Value savedWidth, savedHeight;
  std::unique_ptr<Parts> parts;

  void timerCallback() override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RvvEditor)
};
