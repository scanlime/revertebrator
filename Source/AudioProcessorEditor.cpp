#include "AudioProcessorEditor.h"
#include "MapPanel.h"
#include "WindowPanel.h"

class ParamPanel : public juce::Component {
public:
  ParamPanel(AudioProcessor &p) {
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
      label->setText(param->getName(64), juce::dontSendNotification);
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
  DataPanel(AudioProcessor &p) {
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

struct AudioProcessorEditor::Parts {
  DataPanel data;
  MapPanel map;
  WindowPanel window;
  ParamPanel params;

  Parts(AudioProcessor &p) : data(p), map(p), window(p), params(p) {}
};

AudioProcessorEditor::AudioProcessorEditor(AudioProcessor &p)
    : juce::AudioProcessorEditor(&p), parts(std::make_unique<Parts>(p)) {
  addAndMakeVisible(parts->data);
  addAndMakeVisible(parts->map);
  addAndMakeVisible(parts->window);
  addAndMakeVisible(parts->params);

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
  box.items.add(FlexItem(parts->data).withMinHeight(48));
  box.items.add(FlexItem(parts->map).withFlex(4));
  box.items.add(FlexItem(parts->window).withFlex(1));
  box.items.add(FlexItem(parts->params).withMinHeight(100));
  box.performLayout(getLocalBounds().toFloat());
  savedWidth = getWidth();
  savedHeight = getHeight();
}
