#pragma once

#include "AudioProcessor.h"
#include <JuceHeader.h>

class WindowPanel : public juce::Component {
public:
  WindowPanel(AudioProcessor &);
  ~WindowPanel() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  AudioProcessor &audioProcessor;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WindowPanel)
};
