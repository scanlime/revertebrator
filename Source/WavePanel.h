#pragma once

#include "RvvProcessor.h"
#include <JuceHeader.h>

class WavePanel : public juce::Component {
public:
  WavePanel(RvvProcessor &);
  ~WavePanel() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  RvvProcessor &processor;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WavePanel)
};
