#pragma once

#include "AudioProcessor.h"
#include <JuceHeader.h>

class ParamPanel : public juce::Component {
public:
  ParamPanel(AudioProcessor &);
  ~ParamPanel() override;
  void resized() override;

private:
  juce::OwnedArray<juce::Label> labels;
  juce::OwnedArray<juce::Slider> knobs;
  juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment>
      knobAttach;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParamPanel)
};
