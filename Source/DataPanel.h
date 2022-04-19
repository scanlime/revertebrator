#pragma once

#include "AudioProcessor.h"
#include <JuceHeader.h>

class DataPanel : public juce::Component {
public:
  DataPanel(AudioProcessor &);
  ~DataPanel() override;
  void resized() override;

private:
  AudioProcessor &audioProcessor;

  juce::Label info;
  juce::FilenameComponent filename{{},    {},       true, false,
                                   false, "*.json", "",   "Choose a data file"};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DataPanel)
};
