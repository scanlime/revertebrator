#include "PluginProcessor.h"
#include "PluginEditor.h"

RvAudioProcessor::RvAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput(
          "Output", juce::AudioChannelSet::stereo(), true)),
      state(*this, nullptr, "state", {}) {}

RvAudioProcessor::~RvAudioProcessor() {}

const juce::String RvAudioProcessor::getName() const { return JucePlugin_Name; }

bool RvAudioProcessor::acceptsMidi() const { return true; }

bool RvAudioProcessor::producesMidi() const { return false; }

bool RvAudioProcessor::isMidiEffect() const { return false; }

double RvAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int RvAudioProcessor::getNumPrograms() { return 1; }

int RvAudioProcessor::getCurrentProgram() { return 0; }

void RvAudioProcessor::setCurrentProgram(int index) {}

const juce::String RvAudioProcessor::getProgramName(int index) { return {}; }

void RvAudioProcessor::changeProgramName(int index,
                                         const juce::String &newName) {}

void RvAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {}

void RvAudioProcessor::releaseResources() {}

bool RvAudioProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;
  return true;
}

void RvAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                    juce::MidiBuffer &midiMessages) {
  juce::ScopedNoDenormals noDenormals;
  auto totalNumOutputChannels = getTotalNumOutputChannels();

  for (auto i = 0; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());
}

bool RvAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor *RvAudioProcessor::createEditor() {
  return new RvAudioProcessorEditor(*this);
}

void RvAudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
  if (auto xml = state.copyState().createXml())
    copyXmlToBinary(*xml, destData);
}

void RvAudioProcessor::setStateInformation(const void *data, int sizeInBytes) {
  if (auto xml = getXmlFromBinary(data, sizeInBytes))
    state.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new RvAudioProcessor();
}
