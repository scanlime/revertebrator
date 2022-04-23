#include "ParamPanel.h"

using juce::FlexBox;
using juce::FlexItem;
using juce::Label;
using juce::Slider;
using juce::String;

static const String knobOrder[] = {"grain_width", "grain_rate", "sel_center",
                                   "sel_mod",     "sel_spread", "pitch_spread"};

ParamPanel::ParamPanel(AudioProcessor &p) {
  for (auto item : knobOrder) {

    auto knob = new Slider();
    knob->setSliderStyle(Slider::Rotary);
    knob->setTextBoxStyle(Slider::TextEntryBoxPosition::TextBoxBelow, false, 70,
                          15);
    addAndMakeVisible(*knob);
    knobs.add(knob);

    auto label = new Label();
    label->attachToComponent(knob, false);
    label->setText(p.state.getParameter(item)->getName(64),
                   juce::dontSendNotification);
    label->setJustificationType(juce::Justification::centred);
    addAndMakeVisible(*label);
    labels.add(label);

    knobAttach.add(new juce::AudioProcessorValueTreeState::SliderAttachment(
        p.state, item, *knob));
  }
}

ParamPanel::~ParamPanel() {}

void ParamPanel::resized() {
  FlexBox outer;
  FlexBox inner;
  outer.flexDirection = FlexBox::Direction::column;
  outer.justifyContent = FlexBox::JustifyContent::flexEnd;
  inner.justifyContent = FlexBox::JustifyContent::center;
  for (auto item : knobs) {
    inner.items.add(FlexItem(*item).withFlex(1).withMaxWidth(100));
  }
  outer.items.add(FlexItem(inner).withMinHeight(80));
  outer.performLayout(getLocalBounds().toFloat());
}
