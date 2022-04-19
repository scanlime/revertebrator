#pragma once

#include "PluginProcessor.h"
#include <JuceHeader.h>

class RvDataPanel : public juce::Component {
public:
  RvDataPanel(RvAudioProcessor &);
  ~RvDataPanel() override;
  void resized() override;

private:
  RvAudioProcessor &audioProcessor;

  juce::Label info;
  juce::FilenameComponent filename{{},    {},       true, false,
                                   false, "*.json", "",   "Choose a data file"};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RvDataPanel)
};

class RvMapPanel : public juce::Component {
public:
  RvMapPanel(RvAudioProcessor &);
  ~RvMapPanel() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  RvAudioProcessor &audioProcessor;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RvMapPanel)
};

class RvWindowPanel : public juce::Component {
public:
  RvWindowPanel(RvAudioProcessor &);
  ~RvWindowPanel() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  RvAudioProcessor &audioProcessor;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RvWindowPanel)
};

class RvParamPanel : public juce::Component {
public:
  RvParamPanel(RvAudioProcessor &);
  ~RvParamPanel() override;
  void resized() override;

private:
  RvAudioProcessor &audioProcessor;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RvParamPanel)
};

class RvAudioProcessorEditor : public juce::AudioProcessorEditor {
public:
  RvAudioProcessorEditor(RvAudioProcessor &);
  ~RvAudioProcessorEditor() override;
  void paint(juce::Graphics &) override;
  void resized() override;

private:
  RvDataPanel dataPanel;
  RvMapPanel mapPanel;
  RvWindowPanel windowPanel;
  RvParamPanel paramPanel;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RvAudioProcessorEditor)
};
