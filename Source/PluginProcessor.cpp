#include "PluginProcessor.h"
#include "PluginEditor.h"

RevertebratorAudioProcessor::RevertebratorAudioProcessor()
     : AudioProcessor(BusesProperties()
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

RevertebratorAudioProcessor::~RevertebratorAudioProcessor()
{
}

const juce::String RevertebratorAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool RevertebratorAudioProcessor::acceptsMidi() const
{
    return true;
}

bool RevertebratorAudioProcessor::producesMidi() const
{
    return false;
}

bool RevertebratorAudioProcessor::isMidiEffect() const
{
    return false;
}

double RevertebratorAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int RevertebratorAudioProcessor::getNumPrograms()
{
    return 1;
}

int RevertebratorAudioProcessor::getCurrentProgram()
{
    return 0;
}

void RevertebratorAudioProcessor::setCurrentProgram(int index)
{
}

const juce::String RevertebratorAudioProcessor::getProgramName(int index)
{
    return {};
}

void RevertebratorAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
}

void RevertebratorAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
}

void RevertebratorAudioProcessor::releaseResources()
{
}

bool RevertebratorAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void RevertebratorAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = 0; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());
}

bool RevertebratorAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* RevertebratorAudioProcessor::createEditor()
{
    return new RevertebratorAudioProcessorEditor(*this);
}

void RevertebratorAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
}

void RevertebratorAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RevertebratorAudioProcessor();
}
