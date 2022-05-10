#pragma once

#include "GrainData.h"
#include "GrainSynth.h"
#include <JuceHeader.h>

class RvvProcessor : public juce::AudioProcessor,
                     private juce::ValueTree::Listener,
                     private juce::Value::Listener {
public:
  RvvProcessor();
  ~RvvProcessor() override;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;

  bool isBusesLayoutSupported(const BusesLayout &layouts) const override;

  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;

  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override;

  const juce::String getName() const override;

  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;
  double getTailLengthSeconds() const override;

  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int index) override;
  const juce::String getProgramName(int index) override;
  void changeProgramName(int index, const juce::String &newName) override;

  void getStateInformation(juce::MemoryBlock &destData) override;
  void setStateInformation(const void *data, int sizeInBytes) override;

  juce::AudioProcessorValueTreeState state;
  juce::MidiKeyboardState midiState;
  juce::ThreadPool generalPurposeThreads{2};
  GrainData grainData;

private:
  void attachToState();
  void updateSoundFromState();
  void valueChanged(juce::Value &) override;
  void valueTreePropertyChanged(juce::ValueTree &,
                                const juce::Identifier &) override;

  GrainSynth synth;
  juce::Value grainDataStatus;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RvvProcessor)
};
