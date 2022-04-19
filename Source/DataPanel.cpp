#include "DataPanel.h"

DataPanel::DataPanel(AudioProcessor &p) : audioProcessor(p) {
  addAndMakeVisible(info);
  addAndMakeVisible(filename);
  info.setJustificationType(juce::Justification::topLeft);
}

DataPanel::~DataPanel() {}

void DataPanel::resized() {
  auto area = getLocalBounds();
  filename.setBounds(area.removeFromTop(26));
  info.setBounds(area);
}
