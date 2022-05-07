#include "AudioProcessor.h"
#include "AudioProcessorEditor.h"
#include "GrainSynth.h"

AudioProcessor::AudioProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput(
          "Output", juce::AudioChannelSet::stereo())),
      state(*this, nullptr, "state",
            {std::make_unique<juce::AudioParameterFloat>(
                 "win_width0", "Win A",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.1f),
             std::make_unique<juce::AudioParameterFloat>(
                 "win_width1", "Win B",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.1f),
             std::make_unique<juce::AudioParameterFloat>(
                 "win_phase1", "Phase B",
                 juce::NormalisableRange<float>(-1.0f, 1.0f), 0.f),
             std::make_unique<juce::AudioParameterFloat>(
                 "win_mix", "Mix AB",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
             std::make_unique<juce::AudioParameterFloat>(
                 "grain_rate", "Gr Rate",
                 juce::NormalisableRange<float>(0.01f, 100.0f), 10.f),
             std::make_unique<juce::AudioParameterFloat>(
                 "speed_warp", "Spd Warp",
                 juce::NormalisableRange<float>(0.1f, 4.f), 1.f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_center", "Sel",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_mod", "S Mod", juce::NormalisableRange<float>(0.0f, 1.0f),
                 1.0f),
             std::make_unique<juce::AudioParameterFloat>(
                 "sel_spread", "S Spread",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f),
             std::make_unique<juce::AudioParameterFloat>(
                 "pitch_spread", "P Spread",
                 juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f)}),
      grainData(generalPurposeThreads), synth(128) {

  constexpr auto width = 800;
  constexpr auto height = 400;
  state.state.appendChild(
      {"editor_window", {{"width", width}, {"height", height}}, {}}, nullptr);
  state.state.appendChild({"grain_data", {{"src", ""}}, {}}, nullptr);
  state.state.appendChild({"recent_files", {}, {}}, nullptr);
  attachToState();

  grainData.referToStatusOutput(grainDataStatus);
  grainDataStatus.addListener(this);
}

AudioProcessor::~AudioProcessor() {}
const juce::String AudioProcessor::getName() const { return JucePlugin_Name; }
bool AudioProcessor::hasEditor() const { return true; }
bool AudioProcessor::acceptsMidi() const { return true; }
bool AudioProcessor::producesMidi() const { return false; }
bool AudioProcessor::isMidiEffect() const { return false; }
double AudioProcessor::getTailLengthSeconds() const { return 0.0; }
int AudioProcessor::getNumPrograms() { return 1; }
int AudioProcessor::getCurrentProgram() { return 0; }
void AudioProcessor::setCurrentProgram(int index) {}
const juce::String AudioProcessor::getProgramName(int index) { return {}; }
void AudioProcessor::changeProgramName(int index, const juce::String &name) {}
void AudioProcessor::releaseResources() {}

void AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  synth.setCurrentPlaybackSampleRate(sampleRate);
  updateSoundFromState();
}

bool AudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;
  return true;
}

void AudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                  juce::MidiBuffer &midiMessages) {
  buffer.clear();
  synth.renderNextBlock(buffer, midiMessages, 0, buffer.getNumSamples());
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
  attachToState();
  updateSoundFromState();
}

void AudioProcessor::valueTreePropertyChanged(juce::ValueTree &,
                                              const juce::Identifier &) {
  // Something changed in the state tree, assume it affects sound parameters
  updateSoundFromState();
}

void AudioProcessor::valueChanged(juce::Value &) {
  // Grain data status change, we may have a new index
  updateSoundFromState();
}

void AudioProcessor::attachToState() {
  grainData.referFileInputTo(state.state.getChildWithName("grain_data")
                                 .getPropertyAsValue("src", nullptr));
  state.state.addListener(this);
}

void AudioProcessor::updateSoundFromState() {
  GrainIndex::Ptr index = grainData.getIndex();
  if (index != nullptr) {
    synth.changeSound(
        *index,
        {.sampleRate = synth.getSampleRate(),
         .grainRate = state.getParameterAsValue("grain_rate").getValue(),
         .window =
             {
                 .mix = state.getParameterAsValue("win_mix").getValue(),
                 .width0 = state.getParameterAsValue("win_width0").getValue(),
                 .width1 = state.getParameterAsValue("win_width1").getValue(),
                 .phase1 = state.getParameterAsValue("win_phase1").getValue(),
             },
         .sequence = {
             .selCenter = state.getParameterAsValue("sel_center").getValue(),
             .selMod = state.getParameterAsValue("sel_mod").getValue(),
             .selSpread = state.getParameterAsValue("sel_spread").getValue(),
             .speedWarp = state.getParameterAsValue("speed_warp").getValue(),
             .pitchSpread =
                 state.getParameterAsValue("pitch_spread").getValue(),
         }});
  }
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new AudioProcessor();
}
