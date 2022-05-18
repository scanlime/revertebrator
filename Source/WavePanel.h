#pragma once

#include "RvvProcessor.h"
#include <JuceHeader.h>

class WavePanel : public juce::Component, private juce::ChangeListener {
public:
  WavePanel(RvvProcessor &);
  ~WavePanel() override;

  void paint(juce::Graphics &) override;
  void resized() override;

private:
  class RenderThread;
  class ImageBuilder;

  RvvProcessor &processor;
  std::unique_ptr<RenderThread> thread;

  void changeListenerCallback(juce::ChangeBroadcaster *) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WavePanel)
};
