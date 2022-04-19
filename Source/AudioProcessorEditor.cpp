#include "AudioProcessorEditor.h"

AudioProcessorEditor::AudioProcessorEditor(AudioProcessor &p)
    : juce::AudioProcessorEditor(&p), dataPanel(p), mapPanel(p), windowPanel(p),
      paramPanel(p) {
  addAndMakeVisible(dataPanel);
  addAndMakeVisible(mapPanel);
  addAndMakeVisible(windowPanel);
  addAndMakeVisible(paramPanel);
  setSize(450, 450);
  setResizable(true, true);
  setResizeLimits(400, 200, 1600, 1600);
}

AudioProcessorEditor::~AudioProcessorEditor() {}

void AudioProcessorEditor::paint(juce::Graphics &g) {
  g.fillAll(
      getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioProcessorEditor::resized() {
  auto area = getLocalBounds();
  dataPanel.setBounds(area.removeFromTop(100));
  paramPanel.setBounds(area.removeFromBottom(100));
  windowPanel.setBounds(area.removeFromBottom(50));
  mapPanel.setBounds(area);
}
