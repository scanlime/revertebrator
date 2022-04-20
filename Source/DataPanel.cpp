#include "DataPanel.h"

using juce::FlexBox;
using juce::FlexItem;

DataPanel::DataPanel(AudioProcessor &p) : audioProcessor(p) {
  grainDataSrc.referTo(p.state.state.getChildWithName("grain_data")
                           .getPropertyAsValue("src", nullptr));
  grainDataSrc.addListener(this);
  filename.addListener(this);
  valueChanged(grainDataSrc);
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

void DataPanel::filenameComponentChanged(juce::FilenameComponent *) {
  grainDataSrc.setValue(filename.getCurrentFile().getFullPathName());
}

void DataPanel::valueChanged(juce::Value &) {
  filename.setCurrentFile(grainDataSrc.toString(),
                          juce::NotificationType::dontSendNotification);
}
