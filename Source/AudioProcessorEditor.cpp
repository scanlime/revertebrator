#include "AudioProcessorEditor.h"

AudioProcessorEditor::AudioProcessorEditor(AudioProcessor &p)
    : juce::AudioProcessorEditor(&p), dataPanel(p), mapPanel(p), windowPanel(p),
      paramPanel(p) {
  addAndMakeVisible(dataPanel);
  addAndMakeVisible(mapPanel);
  addAndMakeVisible(windowPanel);
  addAndMakeVisible(paramPanel);

  auto state = p.state.state.getChildWithName("editor_window");
  savedWidth.referTo(state.getPropertyAsValue("width", nullptr));
  savedHeight.referTo(state.getPropertyAsValue("height", nullptr));
  setSize(savedWidth.getValue(), savedHeight.getValue());
  setResizable(true, true);
}

AudioProcessorEditor::~AudioProcessorEditor() {}

void AudioProcessorEditor::paint(juce::Graphics &g) {
  g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioProcessorEditor::resized() {
  using juce::FlexBox;
  using juce::FlexItem;
  FlexBox box;
  box.flexDirection = FlexBox::Direction::column;
  box.items.add(FlexItem(dataPanel).withMinHeight(48));
  box.items.add(FlexItem(mapPanel).withFlex(4));
  box.items.add(FlexItem(windowPanel).withFlex(1));
  box.items.add(FlexItem(paramPanel).withMinHeight(100));
  box.performLayout(getLocalBounds().toFloat());
  savedWidth = getWidth();
  savedHeight = getHeight();
}
