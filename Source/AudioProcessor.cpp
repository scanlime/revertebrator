#include "AudioProcessor.h"
#include "AudioProcessorEditor.h"

using juce::String;

AudioProcessor::AudioProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput(
          "Output", juce::AudioChannelSet::stereo(), true)),
      state(*this, nullptr, "state",
            {std::make_unique<juce::AudioParameterFloat>(
                 "grain_width", "Grain Width",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.1f),
             std::make_unique<juce::AudioParameterFloat>(
                 "grain_rate", "Grain Rate",
                 juce::NormalisableRange<float>(0.0f, 1000.0f), 100.f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_center", "Selector Center",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_mod", "Selector Modulation",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_spread", "Selector Spread",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f),
             std::make_unique<juce::AudioParameterFloat>(
                 "pitch_spread", "Pitch Spread",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f)}) {

  state.state.addChild({"grain_data", {{"src", ""}}, {}}, -1, nullptr);
  state.state.addChild({"ui_state",
                        {
                            {"width", 450},
                            {"height", 450},
                        },
                        {}},
                       -1, nullptr);
}

AudioProcessor::~AudioProcessor() {}

const String AudioProcessor::getName() const { return JucePlugin_Name; }
bool AudioProcessor::acceptsMidi() const { return true; }
bool AudioProcessor::producesMidi() const { return false; }
bool AudioProcessor::isMidiEffect() const { return false; }
double AudioProcessor::getTailLengthSeconds() const { return 0.0; }
int AudioProcessor::getNumPrograms() { return 1; }
int AudioProcessor::getCurrentProgram() { return 0; }
void AudioProcessor::setCurrentProgram(int index) {}
const String AudioProcessor::getProgramName(int index) { return {}; }
void AudioProcessor::changeProgramName(int index, const String &newName) {}

void AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {}

void AudioProcessor::releaseResources() {}

bool AudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;
  return true;
}

void AudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                  juce::MidiBuffer &midiMessages) {
  juce::ScopedNoDenormals noDenormals;
  auto totalNumOutputChannels = getTotalNumOutputChannels();

  for (auto i = 0; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());
}

bool AudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor *AudioProcessor::createEditor() {
  return new AudioProcessorEditor(*this);
}

void AudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
  if (auto xml = state.copyState().createXml())
    copyXmlToBinary(*xml, destData);
}

void AudioProcessor::setStateInformation(const void *data, int sizeInBytes) {
  if (auto xml = getXmlFromBinary(data, sizeInBytes))
    state.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new AudioProcessor();
}