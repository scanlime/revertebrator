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

void MapPanel::mouseMove(const juce::MouseEvent &event) {
  const auto width = getWidth(), height = getHeight();
  GrainData::Accessor gda(audioProcessor.grainData);

  if (width > 0 && height > 0 && gda.numBins() > 0 && gda.numGrains() > 0) {
    int bin = event.x / float(width) * gda.numBins();
    auto gr = gda.grainsForBin(bin);
    int g = gr.getStart() + event.y / float(height) * gr.getLength();
    audioProcessor.temp_ptr = gda.centerSampleForGrain(g);
  }
}

void MapPanel::renderImage() {
  const auto width = getWidth(), height = getHeight();
  mapImage = std::make_unique<Image>(Image::RGB, width, height, false);
  auto bg = findColour(juce::ResizableWindow::backgroundColourId);
  GrainData::Accessor gda(audioProcessor.grainData);

  if (width > 0 && height > 0 && gda.numBins() > 0 && gda.numGrains() > 0) {
    Image::BitmapData bits(*mapImage, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < width; x++) {
      int bin = x / float(width) * gda.numBins();
      auto gr = gda.grainsForBin(bin);
      for (int y = 0; y < height; y++) {
        int g = gr.getStart() + y / float(height) * gr.getLength();
        auto iValue = gda.centerSampleForGrain(g);
        float fValue = iValue / double(gda.numSamples());
        auto color = bg.contrasting(fValue);
        bits.setPixelColour(x, y, color);
      }
    }
  } else {
    mapImage->clear(mapImage->getBounds(), bg);
  }
}
