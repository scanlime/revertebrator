#pragma once

#include "AudioProcessor.h"
#include <JuceHeader.h>

class AudioProcessorEditor : public juce::AudioProcessorEditor {
public:
  AudioProcessorEditor(AudioProcessor &);
  ~AudioProcessorEditor() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  struct Parts;

  juce::Value savedWidth, savedHeight;
  std::unique_ptr<Parts> parts;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioProcessorEditor)
};
