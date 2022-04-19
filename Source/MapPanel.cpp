#include "MapPanel.h"

MapPanel::MapPanel(AudioProcessor &p) : audioProcessor(p) {}

MapPanel::~MapPanel() {}

void MapPanel::paint(juce::Graphics &g) { g.fillAll(juce::Colours::black); }

void MapPanel::resized() {}
