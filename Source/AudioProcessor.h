#pragma once

#include "GrainData.h"
#include <JuceHeader.h>

class AudioProcessor : public juce::AudioProcessor {
public:
  AudioProcessor();
  ~AudioProcessor() override;

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
  juce::ThreadPool generalPurposeThreads{2};
  GrainData grainData;

  juce::int64 temp_ptr{0}, temp_ptr_prev{0}, temp_playback{0};

private:
  void attachState();

  double outputSampleRate{48000};
  juce::OwnedArray<juce::WindowedSincInterpolator> outputResampler;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioProcessor)
};
