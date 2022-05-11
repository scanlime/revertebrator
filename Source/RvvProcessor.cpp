#include "RvvProcessor.h"
#include "GrainSynth.h"
#include "RvvEditor.h"

RvvProcessor::RvvProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput(
          "Output", juce::AudioChannelSet::stereo())),
      state(*this, nullptr, "state",
            {
                std::make_unique<juce::AudioParameterFloat>(
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
                    "grain_rate", "Grain Hz",
                    juce::NormalisableRange<float>(0.0f, 200.0f), 10.f),
                std::make_unique<juce::AudioParameterFloat>(
                    "speed_warp", "Speed",
                    juce::NormalisableRange<float>(0.1f, 2.0f), 1.f),
                std::make_unique<juce::AudioParameterFloat>(
                    "sel_center", "Sel",
                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
                std::make_unique<juce::AudioParameterFloat>(
                    "sel_mod", "S Mod",
                    juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f),
                std::make_unique<juce::AudioParameterFloat>(
                    "sel_spread", "S Spread",
                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f),
                std::make_unique<juce::AudioParameterFloat>(
                    "pitch_spread", "P Spread",
                    juce::NormalisableRange<float>(0.0f, 8.0f), 0.0f),
                std::make_unique<juce::AudioParameterFloat>(
                    "pitch_bend_range", "P Bend",
                    juce::NormalisableRange<float>(1.0f, 64.0f), 12.0f),
                std::make_unique<juce::AudioParameterFloat>(
                    "gain_db_low", "Vol Lo",
                    juce::NormalisableRange<float>(-100.0f, 0.0f), -70.0f),
                std::make_unique<juce::AudioParameterFloat>(
                    "gain_db_high", "Vol Hi",
                    juce::NormalisableRange<float>(-100.0f, 0.0f), -30.0f),
            }),
      grainData(generalPurposeThreads), synth(grainData, 128) {

  state.state.appendChild({"grain_data", {{"src", ""}}, {}}, nullptr);
  state.state.appendChild({"recent_files", {}, {}}, nullptr);
  state.state.appendChild(
      {
          "editor_window",
          {{"width", RvvEditor::defaultWidth},
           {"height", RvvEditor::defaultHeight}},
          {},
      },
      nullptr);

  attachToState();
  grainData.referToStatusOutput(grainDataStatus);
  grainDataStatus.addListener(this);
}

RvvProcessor::~RvvProcessor() {}
const juce::String RvvProcessor::getName() const { return JucePlugin_Name; }
bool RvvProcessor::hasEditor() const { return true; }
bool RvvProcessor::acceptsMidi() const { return true; }
bool RvvProcessor::producesMidi() const { return false; }
bool RvvProcessor::isMidiEffect() const { return false; }
double RvvProcessor::getTailLengthSeconds() const { return 0.0; }
int RvvProcessor::getNumPrograms() { return 1; }
int RvvProcessor::getCurrentProgram() { return 0; }
void RvvProcessor::setCurrentProgram(int index) {}
const juce::String RvvProcessor::getProgramName(int index) { return {}; }
void RvvProcessor::changeProgramName(int index, const juce::String &name) {}
void RvvProcessor::releaseResources() {}

void RvvProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  synth.setCurrentPlaybackSampleRate(sampleRate);
  updateSoundFromState();
}

bool RvvProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;
  return true;
}

void RvvProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                juce::MidiBuffer &midiMessages) {
  buffer.clear();
  auto startSample = 0;
  auto numSamples = buffer.getNumSamples();
  midiState.processNextMidiBuffer(midiMessages, startSample, numSamples, true);
  synth.renderNextBlock(buffer, midiMessages, startSample, numSamples);
}

juce::AudioProcessorEditor *RvvProcessor::createEditor() {
  return new RvvEditor(*this);
}

void RvvProcessor::getStateInformation(juce::MemoryBlock &destData) {
  if (auto xml = state.copyState().createXml())
    copyXmlToBinary(*xml, destData);
}

void RvvProcessor::setStateInformation(const void *data, int sizeInBytes) {
  if (auto xml = getXmlFromBinary(data, sizeInBytes))
    state.replaceState(juce::ValueTree::fromXml(*xml));
  attachToState();
  updateSoundFromState();
}

void RvvProcessor::valueTreePropertyChanged(juce::ValueTree &,
                                            const juce::Identifier &) {
  // Something changed in the state tree, assume it affects sound parameters
  updateSoundFromState();
}

void RvvProcessor::valueChanged(juce::Value &) {
  // Grain data status change, we may have a new index
  updateSoundFromState();
}

void RvvProcessor::attachToState() {
  grainData.referFileInputTo(state.state.getChildWithName("grain_data")
                                 .getPropertyAsValue("src", nullptr));
  state.state.addListener(this);
}

void RvvProcessor::updateSoundFromState() {
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
             .pitchBendRange =
                 state.getParameterAsValue("pitch_bend_range").getValue(),
             .gainDbLow = state.getParameterAsValue("gain_db_low").getValue(),
             .gainDbHigh = state.getParameterAsValue("gain_db_high").getValue(),
         }});
  }
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new RvvProcessor();
}