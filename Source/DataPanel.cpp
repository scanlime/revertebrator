#include "DataPanel.h"

using juce::FlexBox;
using juce::FlexItem;

DataPanel::DataPanel(AudioProcessor &p) {
  grainDataSrc.referTo(p.state.state.getChildWithName("grain_data")
                           .getPropertyAsValue("src", nullptr));
  p.grainData.referToStatusOutput(grainDataStatus);
  grainDataSrc.addListener(this);
  grainDataStatus.addListener(this);
  filename.addListener(this);
  valueChanged(grainDataSrc);
  valueChanged(grainDataStatus);
  addAndMakeVisible(info);
  addAndMakeVisible(filename);
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

void DataPanel::valueChanged(juce::Value &v) {
  if (v.refersToSameSourceAs(grainDataSrc)) {
    filename.setCurrentFile(grainDataSrc.toString(),
                            juce::NotificationType::dontSendNotification);
  }
  if (v.refersToSameSourceAs(grainDataStatus)) {
    info.setText(grainDataStatus.toString(),
                 juce::NotificationType::dontSendNotification);
  }
}
