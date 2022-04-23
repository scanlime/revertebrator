#pragma once

#include "AudioProcessor.h"
#include <JuceHeader.h>

class MapPanel : public juce::Component, private juce::Value::Listener {
public:
  MapPanel(AudioProcessor &);
  ~MapPanel() override;

  void paint(juce::Graphics &) override;
  void resized() override;
  void mouseMove(const juce::MouseEvent &) override;

private:
  AudioProcessor &audioProcessor;
  juce::Value grainDataStatus;
  std::unique_ptr<juce::Image> mapImage;

  void renderImage();
  void valueChanged(juce::Value &) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MapPanel)
};
