#include "RvvEditor.h"
#include "MapPanel.h"
#include "WavePanel.h"

class ParamPanel : public juce::Component {
public:
  ParamPanel(RvvProcessor &p) {
    using juce::Label;
    using juce::Slider;

    static const juce::String knobOrder[] = {
        "win_width0", "win_width1",   "win_phase1", "win_mix",    "grain_rate",
        "speed_warp", "pitch_spread", "sel_center", "sel_spread", "sel_mod"};

    for (auto item : knobOrder) {
      auto param = p.state.getParameter(item);
      if (param == nullptr) {
        jassertfalse;
        continue;
      }

      auto knob = new Slider();
      knob->setSliderStyle(Slider::Rotary);
      knob->setTextBoxStyle(Slider::TextEntryBoxPosition::TextBoxBelow, false,
                            70, 15);
      addAndMakeVisible(*knob);
      knobs.add(knob);

      auto label = new Label();
      label->attachToComponent(knob, false);
      label->setText(param->name, juce::dontSendNotification);
      label->setJustificationType(juce::Justification::centred);
      addAndMakeVisible(*label);
      labels.add(label);

      knobAttach.add(new juce::AudioProcessorValueTreeState::SliderAttachment(
          p.state, item, *knob));
    }
  }

  void resized() override {
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

private:
  juce::OwnedArray<juce::Label> labels;
  juce::OwnedArray<juce::Slider> knobs;
  juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment>
      knobAttach;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParamPanel)
};

class DataPanel : public juce::Component,
                  private juce::FilenameComponentListener,
                  private juce::Value::Listener {
public:
  DataPanel(RvvProcessor &p) {
    recentItems = p.state.state.getChildWithName("recent_files");
    juce::StringArray recentStrings;
    for (auto item : recentItems) {
      recentStrings.add(item.getProperty("src"));
    }
    filename.setRecentlyUsedFilenames(recentStrings);

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

  void resized() override {
    using juce::FlexBox;
    using juce::FlexItem;
    FlexBox box;
    box.flexDirection = FlexBox::Direction::column;
    box.items.add(FlexItem(filename).withMinHeight(24));
    box.items.add(FlexItem(info).withFlex(1));
    box.performLayout(getLocalBounds().toFloat());
  }

private:
  juce::ValueTree recentItems;
  juce::Value grainDataSrc, grainDataStatus;
  juce::Label info;
  juce::FilenameComponent filename{
      {}, {}, false, false, false, "*.rvv", "", "Choose grain data..."};

  void filenameComponentChanged(juce::FilenameComponent *) override {
    grainDataSrc.setValue(filename.getCurrentFile().getFullPathName());

    auto recentStrings = filename.getRecentlyUsedFilenames();
    recentItems.removeAllChildren(nullptr);
    for (auto name : recentStrings) {
      recentItems.appendChild({"item", {{"src", name}}, {}}, nullptr);
    }
  }

  void valueChanged(juce::Value &v) override {
    if (v.refersToSameSourceAs(grainDataSrc)) {
      filename.setCurrentFile(grainDataSrc.toString(),
                              juce::NotificationType::dontSendNotification);
    }
    if (v.refersToSameSourceAs(grainDataStatus)) {
      info.setText(grainDataStatus.toString(),
                   juce::NotificationType::dontSendNotification);
    }
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DataPanel)
};

struct RvvEditor::Parts {
  DataPanel data;
  MapPanel map;
  WavePanel wave;
  ParamPanel params;
  juce::MidiKeyboardComponent keyboard;

  Parts(RvvProcessor &p)
      : data(p), map(p), wave(p), params(p),
        keyboard(p.midiState, juce::MidiKeyboardComponent::horizontalKeyboard) {
  }
};

RvvEditor::RvvEditor(RvvProcessor &p)
    : juce::AudioProcessorEditor(&p), parts(std::make_unique<Parts>(p)) {

  addAndMakeVisible(parts->data);
  addAndMakeVisible(parts->map);
  addAndMakeVisible(parts->wave);
  addAndMakeVisible(parts->params);
  addAndMakeVisible(parts->keyboard);

  parts->keyboard.setKeyPressBaseOctave(4);
  parts->keyboard.setLowestVisibleKey(12);

  auto state = p.state.state.getChildWithName("editor_window");
  savedWidth.referTo(state.getPropertyAsValue("width", nullptr));
  savedHeight.referTo(state.getPropertyAsValue("height", nullptr));
  setSize(savedWidth.getValue(), savedHeight.getValue());
  setResizable(true, true);
}

RvvEditor::~RvvEditor() {}

void RvvEditor::paint(juce::Graphics &g) {
  g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
}

void RvvEditor::resized() {
  using juce::FlexBox;
  using juce::FlexItem;
  FlexBox box;
  box.flexDirection = FlexBox::Direction::column;
  box.items.add(FlexItem(parts->data).withMinHeight(48));
  box.items.add(FlexItem(parts->keyboard).withFlex(1));
  box.items.add(FlexItem(parts->map).withFlex(3));
  box.items.add(FlexItem(parts->wave).withFlex(1));
  box.items.add(FlexItem(parts->params).withMinHeight(100));
  box.performLayout(getLocalBounds().toFloat());
  savedWidth = getWidth();
  savedHeight = getHeight();
}
