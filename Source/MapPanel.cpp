#include "MapPanel.h"
#include "GrainData.h"

using juce::Image;
using juce::Rectangle;
using juce::uint64;

class MapLayout {
public:
  MapLayout(const Rectangle<float> &bounds, const GrainData::Accessor &gda)
      : bounds(bounds), gda(gda) {}

  struct PointInfo {
    bool valid;
    unsigned bin, grain;
    uint64 sample;
  };

  PointInfo pointInfo(const juce::Point<float> &p) {
    if (bounds.contains(p)) {
      float relX = (p.x - bounds.getX()) / bounds.getWidth();
      float relY = (p.y - bounds.getY()) / bounds.getHeight();
      auto result = PointInfo{true};
      auto nBins = gda.numBins();
      result.bin = juce::jlimit(0U, nBins - 1, unsigned(nBins * relX));
      auto gr = gda.grainsForBin(result.bin);
      result.grain = gr.clipValue(gr.getStart() + gr.getLength() * relY);
      result.sample = gda.centerSampleForGrain(result.grain);
      return result;
    } else {
      return PointInfo{false};
    }
  }

private:
  Rectangle<float> bounds;
  const GrainData::Accessor &gda;
};

MapPanel::MapPanel(AudioProcessor &p) : audioProcessor(p) {
  p.grainData.referToStatusOutput(grainDataStatus);
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
  GrainData::Accessor gda(audioProcessor.grainData);
  auto layout = MapLayout(getLocalBounds().toFloat(), gda);
  auto point = layout.pointInfo(event.getPosition().toFloat());
  if (point.valid) {
    audioProcessor.temp_ptr = point.sample;
  }
}

void MapPanel::renderImage() {
  const auto bounds = getLocalBounds();
  mapImage = std::make_unique<Image>(Image::RGB, bounds.getWidth(),
                                     bounds.getHeight(), false);
  if (bounds.isEmpty()) {
    return;
  }

  auto bg = findColour(juce::ResizableWindow::backgroundColourId);
  GrainData::Accessor gda(audioProcessor.grainData);
  auto layout = MapLayout(bounds.toFloat(), gda);
  Image::BitmapData bits(*mapImage, juce::Image::BitmapData::writeOnly);

  for (int y = 0; y < bounds.getHeight(); y++) {
    for (int x = 0; x < bounds.getWidth(); x++) {
      auto point = layout.pointInfo(juce::Point<float>(x, y));
      float v = point.valid ? (point.sample / double(gda.numSamples())) : 0.f;
      bits.setPixelColour(x, y, bg.contrasting(v));
    }
  }
}
