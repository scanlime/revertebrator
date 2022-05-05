#include "AudioProcessor.h"
#include "AudioProcessorEditor.h"

using juce::String;

AudioProcessor::AudioProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput(
          "Output", juce::AudioChannelSet::stereo())),
      state(*this, nullptr, "state",
            {std::make_unique<juce::AudioParameterFloat>(
                 "win_width0", "Win A",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
             std::make_unique<juce::AudioParameterFloat>(
                 "win_width1", "Win B",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
             std::make_unique<juce::AudioParameterFloat>(
                 "win_phase1", "Phase B",
                 juce::NormalisableRange<float>(-1.0f, 1.0f), 0.f),
             std::make_unique<juce::AudioParameterFloat>(
                 "win_mix", "Mix AB",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
             std::make_unique<juce::AudioParameterFloat>(
                 "grain_rate", "Grain Rate",
                 juce::NormalisableRange<float>(0.0f, 100.0f), 10.f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_center", "Sel Center",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_mod", "Sel Mod",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_spread", "S.Spread",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f),
             std::make_unique<juce::AudioParameterFloat>(
                 "pitch_spread", "P.Spread",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f)}),
      grainData(generalPurposeThreads) {

  constexpr auto width = 800;
  constexpr auto height = 400;

  state.state.appendChild({"grain_data", {{"src", ""}}, {}}, nullptr);
  state.state.appendChild({"recent_files", {}, {}}, nullptr);
  state.state.appendChild(
      {"editor_window", {{"width", width}, {"height", height}}, {}}, nullptr);

  attachState();
}

AudioProcessor::~AudioProcessor() {}

const String AudioProcessor::getName() const { return JucePlugin_Name; }
bool AudioProcessor::hasEditor() const { return true; }
bool AudioProcessor::acceptsMidi() const { return true; }
bool AudioProcessor::producesMidi() const { return false; }
bool AudioProcessor::isMidiEffect() const { return false; }
double AudioProcessor::getTailLengthSeconds() const { return 0.0; }

int AudioProcessor::getNumPrograms() { return 1; }
int AudioProcessor::getCurrentProgram() { return 0; }
void AudioProcessor::setCurrentProgram(int index) {}
const String AudioProcessor::getProgramName(int index) { return {}; }
void AudioProcessor::changeProgramName(int index, const String &newName) {}

void AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  outputSampleRate = sampleRate;
}

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
  buffer.clear(0, buffer.getNumSamples());

  auto index = grainData.getIndex();
  if (index) {
    auto wave = grainData.getWaveform(
        *index,
        GrainWaveform::Key{.grain = temp_grain,
                           .speedRatio = 0.2f,
                           .window = GrainWaveform::Window{
                               index->maxGrainWidthSamples(), 0, 1, 0, 0}});
    if (wave) {
      for (int i = 0; i < buffer.getNumSamples(); i++) {
        temp_sample = (temp_sample + 1) % wave->buffer.getNumSamples();
        float f = wave->buffer.getSample(0, temp_sample);
        for (int ch = 0; ch < buffer.getNumChannels(); ch++) {
          buffer.setSample(ch, i, f);
        }
      }
    }
  }
}

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
  attachState();
}

void AudioProcessor::attachState() {
  grainData.referFileInputTo(state.state.getChildWithName("grain_data")
                                 .getPropertyAsValue("src", nullptr));
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new AudioProcessor();
}
