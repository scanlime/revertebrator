#include "DataPanel.h"

using juce::FlexBox;
using juce::FlexItem;

DataPanel::DataPanel(AudioProcessor &p) : audioProcessor(p) {
  addAndMakeVisible(info);
  addAndMakeVisible(filename);
  info.setJustificationType(juce::Justification::topLeft);
}

DataPanel::~DataPanel() {}

void DataPanel::resized() {
  FlexBox box;
  box.flexDirection = FlexBox::Direction::column;
  box.items.add(FlexItem(filename).withMinHeight(24));
  box.items.add(FlexItem(info).withFlex(1));
  box.performLayout(getLocalBounds().toFloat());
}
