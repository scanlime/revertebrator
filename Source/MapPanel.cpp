#include "MapPanel.h"

MapPanel::MapPanel(AudioProcessor &p) : audioProcessor(p) {
  grainDataStatus.referTo(p.grainData.status);
  grainDataStatus.addListener(this);
}

MapPanel::~MapPanel() {}

void MapPanel::paint(juce::Graphics &g) {
  if (!mapImage || mapImage->getBounds() != getLocalBounds()) {
    renderImage();
  }
  jassert(mapImage && mapImage->getBounds() == getLocalBounds());
  g.drawImageAt(*mapImage, 0, 0);
}

void MapPanel::resized() { mapImage = nullptr; }

void MapPanel::valueChanged(juce::Value &) {
  mapImage = nullptr;
  repaint();
}

void MapPanel::renderImage() {
  const auto width = getWidth(), height = getHeight();
  mapImage =
      std::make_unique<juce::Image>(juce::Image::RGB, width, height, false);
  auto background =
      getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
  juce::Image::BitmapData bits(*mapImage, juce::Image::BitmapData::writeOnly);
  GrainData::Accessor grainData(audioProcessor.grainData);

  for (int x = 0; x < width; x++) {
    int bin = x / float(width) * grainData.numBins();
    auto gr = grainData.grainsForBin(bin);
    for (int y = 0; y < height; y++) {
      int g = gr.getStart() + y / float(height) * gr.getLength();
      float value =
          grainData.centerSampleForGrain(g) / double(grainData.numSamples());
      bits.setPixelColour(x, y, background.contrasting(value));
    }
  }
}
