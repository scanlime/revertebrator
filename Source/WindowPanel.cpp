#include "WindowPanel.h"

WindowPanel::WindowPanel(AudioProcessor &p) : audioProcessor(p) {}

WindowPanel::~WindowPanel() {}

void WindowPanel::paint(juce::Graphics &g) { g.fillAll(juce::Colours::white); }

void WindowPanel::resized() {}
