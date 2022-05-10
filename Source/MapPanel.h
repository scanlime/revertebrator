#pragma once

#include "RvvProcessor.h"
#include <JuceHeader.h>

class MapPanel : public juce::Component,
                 private juce::Value::Listener,
                 private juce::ChangeListener {
public:
  MapPanel(RvvProcessor &);
  ~MapPanel() override;

  void paint(juce::Graphics &) override;
  void resized() override;

private:
  class Layout;
  class ImageRender;

  RvvProcessor &processor;
  std::unique_ptr<ImageRender> image;
  juce::Value grainDataStatus;

  void requestNewImage();
  void valueChanged(juce::Value &) override;
  void changeListenerCallback(juce::ChangeBroadcaster *) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MapPanel)
};
