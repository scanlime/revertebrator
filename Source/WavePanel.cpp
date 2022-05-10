#include "WavePanel.h"

WavePanel::WavePanel(RvvProcessor &p) : processor(p) {}
WavePanel::~WavePanel() {}

void WavePanel::resized() {}

void WavePanel::paint(juce::Graphics &g) {
  g.fillAll(juce::Colour(0xFFFFFF00));
}
