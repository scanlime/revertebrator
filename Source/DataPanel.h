#pragma once

#include "AudioProcessor.h"
#include <JuceHeader.h>

class DataPanel : public juce::Component,
                  private juce::FilenameComponentListener,
                  private juce::Value::Listener {
public:
  DataPanel(AudioProcessor &);
  ~DataPanel() override;
  void resized() override;

private:
  AudioProcessor &audioProcessor;
  juce::Value grainDataSrc;
  juce::Label info;
  juce::FilenameComponent filename{{},    {},       true, false,
                                   false, "*.json", "",   "Choose a data file"};

  void filenameComponentChanged(juce::FilenameComponent *) override;
  void valueChanged(juce::Value &) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DataPanel)
};
