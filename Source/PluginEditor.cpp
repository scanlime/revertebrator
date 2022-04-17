#include "PluginProcessor.h"
#include "PluginEditor.h"

RevertebratorAudioProcessorEditor::RevertebratorAudioProcessorEditor(RevertebratorAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(600, 600);
}

RevertebratorAudioProcessorEditor::~RevertebratorAudioProcessorEditor()
{
}

void RevertebratorAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    g.setColour(juce::Colours::white);
    g.setFont(20.0f);
    g.drawFittedText("revertebrator", getLocalBounds(), juce::Justification::centred, 1);
}

void RevertebratorAudioProcessorEditor::resized()
{
}
