#pragma once

#include "GrainSynth.h"
#include "RvvProcessor.h"
#include <JuceHeader.h>

class WavePanel : public juce::Component,
                  private GrainVoice::Listener,
                  private juce::Timer {
public:
  WavePanel(RvvProcessor &);
  ~WavePanel() override;

  void paint(juce::Graphics &) override;
  void resized() override;

private:
  struct WaveInfo {
    using Key = void *;
    GrainWaveform::Ptr ptr;
  };

  RvvProcessor &processor;
  std::mutex wavesMutex;
  juce::HashMap<WaveInfo::Key, WaveInfo> waves;

  void timerCallback() override;
  void grainVoicePlaying(const GrainVoice &voice, const GrainSound &sound,
                         GrainWaveform &wave, const GrainSequence::Point &seq,
                         int sampleNum) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WavePanel)
};
