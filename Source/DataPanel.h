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
  juce::ValueTree recentItems;
  juce::Value grainDataSrc, grainDataStatus;
  juce::Label info;
  juce::FilenameComponent filename{
      {}, {}, false, false, false, "*.rvv", "", "Choose grain data..."};

  void filenameComponentChanged(juce::FilenameComponent *) override;
  void valueChanged(juce::Value &) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DataPanel)
};
