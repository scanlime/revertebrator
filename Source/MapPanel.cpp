#include "MapPanel.h"

using juce::Image;

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
  mapImage = std::make_unique<Image>(Image::RGB, width, height, false);
  auto bg = findColour(juce::ResizableWindow::backgroundColourId);
  GrainData::Accessor grainData(audioProcessor.grainData);

  if (grainData.numBins() > 0 && grainData.numGrains() > 0) {
    Image::BitmapData bits(*mapImage, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < width; x++) {
      int bin = x / float(width) * grainData.numBins();
      auto gr = grainData.grainsForBin(bin);
      for (int y = 0; y < height; y++) {
        int g = gr.getStart() + y / float(height) * gr.getLength();
        auto iValue = grainData.centerSampleForGrain(g);
        float fValue = iValue / double(grainData.numSamples());
        auto color = bg.contrasting(fValue);
        bits.setPixelColour(x, y, color);
      }
    }
  } else {
    mapImage->clear(mapImage->getBounds(), bg);
  }
}
