#include "ParamPanel.h"

static const juce::String knobOrder[] = {
    "win_width0", "win_width1",   "win_phase1", "win_mix",    "grain_rate",
    "speed_warp", "pitch_spread", "sel_center", "sel_spread", "sel_mod"};

ParamPanel::ParamPanel(AudioProcessor &p) {
  using juce::Label;
  using juce::Slider;

  for (auto item : knobOrder) {
    auto param = p.state.getParameter(item);
    if (param == nullptr) {
      jassertfalse;
      continue;
    }

    auto knob = new Slider();
    knob->setSliderStyle(Slider::Rotary);
    knob->setTextBoxStyle(Slider::TextEntryBoxPosition::TextBoxBelow, false, 70,
                          15);
    addAndMakeVisible(*knob);
    knobs.add(knob);

    auto label = new Label();
    label->attachToComponent(knob, false);
    label->setText(param->getName(64), juce::dontSendNotification);
    label->setJustificationType(juce::Justification::centred);
    addAndMakeVisible(*label);
    labels.add(label);

    knobAttach.add(new juce::AudioProcessorValueTreeState::SliderAttachment(
        p.state, item, *knob));
  }
}

ParamPanel::~ParamPanel() {}

void ParamPanel::resized() {
  using juce::FlexBox;
  using juce::FlexItem;
  FlexBox outer, inner;
  outer.flexDirection = FlexBox::Direction::column;
  outer.justifyContent = FlexBox::JustifyContent::flexEnd;
  inner.justifyContent = FlexBox::JustifyContent::center;
  for (auto item : knobs) {
    inner.items.add(FlexItem(*item).withFlex(1).withMaxWidth(100));
  }
  outer.items.add(FlexItem(inner).withMinHeight(80));
  outer.performLayout(getLocalBounds().toFloat());
}
