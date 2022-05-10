#pragma once

#include "RvvProcessor.h"
#include <JuceHeader.h>

class RvvEditor : public juce::AudioProcessorEditor {
public:
  RvvEditor(RvvProcessor &);
  ~RvvEditor() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  struct Parts;

  juce::Value savedWidth, savedHeight;
  std::unique_ptr<Parts> parts;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RvvEditor)
};
