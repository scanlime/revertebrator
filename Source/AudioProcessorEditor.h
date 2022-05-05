#pragma once

#include "AudioProcessor.h"
#include "DataPanel.h"
#include "MapPanel.h"
#include "ParamPanel.h"
#include "WindowPanel.h"
#include <JuceHeader.h>

class AudioProcessorEditor : public juce::AudioProcessorEditor {
public:
  AudioProcessorEditor(AudioProcessor &);
  ~AudioProcessorEditor() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  DataPanel dataPanel;
  MapPanel mapPanel;
  WindowPanel windowPanel;
  ParamPanel paramPanel;

  juce::Value savedWidth, savedHeight;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioProcessorEditor)
};
