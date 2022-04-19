#include "PluginEditor.h"
#include "PluginProcessor.h"

RvDataPanel::RvDataPanel(RvAudioProcessor &p) : audioProcessor(p) {
  addAndMakeVisible(info);
  addAndMakeVisible(filename);
  info.setJustificationType(juce::Justification::topLeft);
}

RvDataPanel::~RvDataPanel() {}

void RvDataPanel::resized() {
  auto area = getLocalBounds();
  filename.setBounds(area.removeFromTop(26));
  info.setBounds(area);
}

RvMapPanel::RvMapPanel(RvAudioProcessor &p) : audioProcessor(p) {}

RvMapPanel::~RvMapPanel() {}

void RvMapPanel::paint(juce::Graphics &g) { g.fillAll(juce::Colours::black); }

void RvMapPanel::resized() {}

RvWindowPanel::RvWindowPanel(RvAudioProcessor &p) : audioProcessor(p) {}

RvWindowPanel::~RvWindowPanel() {}

void RvWindowPanel::paint(juce::Graphics &g) {
  g.fillAll(juce::Colours::white);
}

void RvWindowPanel::resized() {}

RvParamPanel::RvParamPanel(RvAudioProcessor &p) : audioProcessor(p) {}

RvParamPanel::~RvParamPanel() {}

void RvParamPanel::resized() {}

RvAudioProcessorEditor::RvAudioProcessorEditor(RvAudioProcessor &p)
    : AudioProcessorEditor(&p), dataPanel(p), mapPanel(p), windowPanel(p),
      paramPanel(p) {
  addAndMakeVisible(dataPanel);
  addAndMakeVisible(mapPanel);
  addAndMakeVisible(windowPanel);
  addAndMakeVisible(paramPanel);
  setSize(450, 450);
  setResizable(true, true);
  setResizeLimits(400, 200, 1600, 1600);
}

RvAudioProcessorEditor::~RvAudioProcessorEditor() {}

void RvAudioProcessorEditor::paint(juce::Graphics &g) {
  g.fillAll(
      getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void RvAudioProcessorEditor::resized() {
  auto area = getLocalBounds();
  dataPanel.setBounds(area.removeFromTop(100));
  paramPanel.setBounds(area.removeFromBottom(100));
  windowPanel.setBounds(area.removeFromBottom(50));
  mapPanel.setBounds(area);
}
