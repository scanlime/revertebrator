#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class RevertebratorAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    RevertebratorAudioProcessorEditor(RevertebratorAudioProcessor&);
    ~RevertebratorAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    RevertebratorAudioProcessor& audioProcessor;

    juce::TextButton dataFileButton{"Open..."};
    juce::FileChooser dataFileChooser{"Choose a data file...",
	    juce::File::getSpecialLocation(juce::File::SpecialLocationType::currentApplicationFile), "*.json"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RevertebratorAudioProcessorEditor)
};
