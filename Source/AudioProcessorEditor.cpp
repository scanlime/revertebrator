#include "AudioProcessorEditor.h"

using juce::FlexBox;
using juce::FlexItem;

AudioProcessorEditor::AudioProcessorEditor(AudioProcessor &p)
    : juce::AudioProcessorEditor(&p), thread("audio_processor_editor"),
      dataPanel(p), mapPanel(p, thread), windowPanel(p), paramPanel(p) {
  addAndMakeVisible(dataPanel);
  addAndMakeVisible(mapPanel);
  addAndMakeVisible(windowPanel);
  addAndMakeVisible(paramPanel);
  setSize(450, 450);
  setResizable(true, true);
  setResizeLimits(400, 200, 1600, 1600);
  thread.startThread();
}

AudioProcessorEditor::~AudioProcessorEditor() { thread.stopThread(500); }

void AudioProcessorEditor::paint(juce::Graphics &g) {
  g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioProcessorEditor::resized() {
  FlexBox box;
  box.flexDirection = FlexBox::Direction::column;
  box.items.add(FlexItem(dataPanel).withMinHeight(48));
  box.items.add(FlexItem(mapPanel).withFlex(4));
  box.items.add(FlexItem(windowPanel).withFlex(1));
  box.items.add(FlexItem(paramPanel).withMinHeight(100));
  box.performLayout(getLocalBounds().toFloat());
}
