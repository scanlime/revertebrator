#pragma once

#include "GrainSynth.h"
#include "RvvProcessor.h"
#include <JuceHeader.h>

class WavePanel : public juce::Component, private juce::ChangeListener {
public:
  WavePanel(RvvProcessor &);
  ~WavePanel() override;

  void paint(juce::Graphics &) override;
  void resized() override;

private:
  class ImageRender;

  RvvProcessor &processor;
  std::unique_ptr<ImageRender> image;

  void changeListenerCallback(juce::ChangeBroadcaster *) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WavePanel)
};
