#pragma once

#include "AudioProcessor.h"
#include <JuceHeader.h>

class MapPanel : public juce::Component {
public:
  MapPanel(AudioProcessor &);
  ~MapPanel() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  AudioProcessor &audioProcessor;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MapPanel)
};
