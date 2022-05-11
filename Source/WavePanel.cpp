#include "WavePanel.h"

WavePanel::WavePanel(RvvProcessor &p) : processor(p) {
  processor.synth.addListener(this);
  startTimerHz(20);
}

WavePanel::~WavePanel() { processor.synth.removeListener(this); }

void WavePanel::resized() {}

void WavePanel::timerCallback() { repaint(); }

void WavePanel::grainVoicePlaying(const GrainVoice &voice,
                                  const GrainSound &sound, GrainWaveform &wave,
                                  const GrainSequence::Point &seq,
                                  int sampleNum) {

  std::lock_guard<std::mutex> guard(wavesMutex);
  auto &waveInfo = waves.getReference(WaveInfo::Key(&wave));
  waveInfo.ptr = wave;
}

void WavePanel::paint(juce::Graphics &g) {
  juce::HashMap<WaveInfo::Key, WaveInfo> wavesToDraw;
  {
    std::lock_guard<std::mutex> guard(wavesMutex);
    waves.swapWith(wavesToDraw);
  }

  for (auto wave : wavesToDraw) {
    printf("wave %p\n", wave.ptr.get());
  }

  g.fillAll(juce::Colour(0x10000000));
}
