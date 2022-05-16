#include "RvvEditor.h"
#include "MapPanel.h"
#include "WavePanel.h"

class ParamKnob : public juce::Component {
public:
  ParamKnob(RvvProcessor &processor, const juce::String &paramId) {
    auto param = processor.state.getParameter(paramId);
    if (param == nullptr) {
      jassertfalse;
    } else {
      slider = std::make_unique<juce::Slider>();
      slider->setSliderStyle(juce::Slider::Rotary);
      slider->setTextBoxStyle(juce::Slider::TextEntryBoxPosition::TextBoxBelow,
                              false, 70, 18);
      addAndMakeVisible(*slider);

      label = std::make_unique<juce::Label>();
      label->setText(param->name, juce::NotificationType::sendNotification);
      label->setJustificationType(juce::Justification::centred);
      addAndMakeVisible(*label);

      attach = std::make_unique<
          juce::AudioProcessorValueTreeState::SliderAttachment>(
          processor.state, paramId, *slider);
    }
  }

  void resized() override {
    juce::FlexBox box;
    box.flexDirection = juce::FlexBox::Direction::column;
    box.justifyContent = juce::FlexBox::JustifyContent::center;
    box.items.add(juce::FlexItem().withMinHeight(2));
    box.items.add(juce::FlexItem(*label).withMinHeight(15));
    box.items.add(juce::FlexItem(*slider).withFlex(1));
    box.items.add(juce::FlexItem().withMinHeight(2));
    box.performLayout(getLocalBounds().toFloat());
  }

private:
  std::unique_ptr<juce::Label> label;
  std::unique_ptr<juce::Slider> slider;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParamKnob)
};

class ParamPanel : public juce::Component {
public:
  ParamPanel(RvvProcessor &processor,
             const juce::Array<juce::String> knobOrder) {
    for (auto &paramId : knobOrder) {
      auto knob = new ParamKnob(processor, paramId);
      addAndMakeVisible(*knob);
      knobs.add(knob);
    }
  }

  void resized() override {
    juce::FlexBox row;
    row.justifyContent = juce::FlexBox::JustifyContent::center;
    for (auto item : knobs) {
      row.items.add(juce::FlexItem(*item).withFlex(1).withMaxWidth(100));
    }
    row.performLayout(getLocalBounds().toFloat());
  }

private:
  juce::OwnedArray<ParamKnob> knobs;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParamPanel)
};

class StatusPanel : public juce::Component, private juce::Timer {
public:
  StatusPanel(RvvProcessor &p) : grainData(p.grainData) {
    timerCallback();
    addAndMakeVisible(label);
    startTimer(300);
  }

  ~StatusPanel() override {}
  void resized() override { label.setBounds(getLocalBounds()); }

private:
  GrainData &grainData;
  juce::Label label;

  void timerCallback() override {
    auto load = grainData.averageLoadQueueDepth();
    auto index = grainData.getIndex();
    auto cached = index == nullptr ? 0 : index->cache.sizeInBytes();
    auto text = juce::String(load, 2) + " load, " +
                juce::String(cached / float(1024 * 1024), 1) + "MB cache";
    label.setText(text, juce::NotificationType::dontSendNotification);
  }
};

class DataPanel : public juce::Component,
                  private juce::FilenameComponentListener,
                  private juce::Value::Listener {
public:
  DataPanel(RvvProcessor &p) : status(p) {
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
    addAndMakeVisible(status);
  }

  void resized() override {
    juce::FlexBox outer, inner;
    outer.flexDirection = juce::FlexBox::Direction::column;
    outer.items.add(juce::FlexItem(filename).withMinHeight(24));
    outer.items.add(juce::FlexItem(inner).withFlex(1));
    inner.items.add(juce::FlexItem(info).withFlex(3));
    inner.items.add(juce::FlexItem(status).withFlex(1).withMaxWidth(170));
    outer.performLayout(getLocalBounds().toFloat());
  }

private:
  juce::ValueTree recentItems;
  juce::Value grainDataSrc, grainDataStatus;
  juce::Label info;
  StatusPanel status;
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
  juce::OwnedArray<ParamPanel> params;
  juce::MidiKeyboardComponent keyboard;

  Parts(RvvProcessor &p)
      : data(p), map(p), wave(p),
        params({
            new ParamPanel(p,
                           {"win_width0", "win_width1", "win_phase1", "win_mix",
                            "speed_warp", "grain_rate", "grain_rate_spread"}),
            new ParamPanel(p, {"pitch_spread", "pitch_bend_range", "sel_center",
                               "sel_spread", "sel_mod", "gain_db_low",
                               "gain_db_high"}),
        }),
        keyboard(p.midiState, juce::MidiKeyboardComponent::horizontalKeyboard) {
  }
};

RvvEditor::RvvEditor(RvvProcessor &p)
    : juce::AudioProcessorEditor(&p), parts(std::make_unique<Parts>(p)) {

  addAndMakeVisible(parts->data);
  addAndMakeVisible(parts->map);
  addAndMakeVisible(parts->wave);
  addAndMakeVisible(parts->keyboard);
  for (auto &part : parts->params) {
    addAndMakeVisible(part);
  }

  parts->keyboard.setKeyPressBaseOctave(4);
  parts->keyboard.setLowestVisibleKey(3 * 12);
  parts->keyboard.setVelocity(0.7, true);

  auto state = p.state.state.getChildWithName("editor_window");
  savedWidth.referTo(state.getPropertyAsValue("width", nullptr));
  savedHeight.referTo(state.getPropertyAsValue("height", nullptr));
  setSize(savedWidth.getValue(), savedHeight.getValue());
  setResizable(true, true);

  // On Android, asynchronously ask for file read permission
  juce::RuntimePermissions::request(
      juce::RuntimePermissions::readExternalStorage, [](bool wasGranted) {});

  // JUCE's recommended approach for setting initial keyboard focus
  startTimer(400);
}

RvvEditor::~RvvEditor() {}

void RvvEditor::paint(juce::Graphics &g) {
  g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
}

void RvvEditor::timerCallback() {
  if (isVisible() && isShowing()) {
    stopTimer();
    parts->keyboard.grabKeyboardFocus();
  }
}

void RvvEditor::resized() {
  juce::FlexBox box;
  box.flexDirection = juce::FlexBox::Direction::column;
  box.items.add(juce::FlexItem(parts->data).withMinHeight(48));
  box.items.add(juce::FlexItem(parts->map).withFlex(10));
  box.items.add(juce::FlexItem(parts->wave).withFlex(4));
  for (auto &part : parts->params) {
    box.items.add(juce::FlexItem(*part).withMinHeight(80).withFlex(1));
  }
  box.items.add(juce::FlexItem(parts->keyboard).withMinHeight(50).withFlex(1));
  box.performLayout(getLocalBounds().toFloat());
  savedWidth = getWidth();
  savedHeight = getHeight();
}
