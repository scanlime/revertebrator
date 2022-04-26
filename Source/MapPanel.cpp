#include "MapPanel.h"
#include "GrainData.h"
#include <random>

using juce::Image;
using juce::Point;
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

  forcedinline PointInfo pointInfo(const Point<float> &p) {
    if (bounds.contains(p)) {
      auto result = PointInfo{.valid = true};

      // X axis: logarithmic pitch
      float relX = (p.x - bounds.getX()) / (bounds.getWidth() - 1);
      auto pitches = gda.pitchRange();
      if (pitches.isEmpty()) {
        result.bin = 0;
      } else {
        float lowEnd = pitches.getStart();
        float highRatio = pitches.getEnd() / lowEnd;
        float hz = lowEnd * exp(relX * log(highRatio));
        result.bin = gda.closestBinForPitch(hz);
      }

      // Y axis: linear grain selector, variable resolution
      auto gr = gda.grainsForBin(result.bin);
      float relY = (p.y - bounds.getY()) / bounds.getHeight();
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
    printf("grain %d bin %d, %f Hz\n", point.grain, point.bin,
           gda.pitchForBin(point.bin));
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
  std::mt19937 prng;

  for (int y = 0; y < bounds.getHeight(); y++) {
    for (int x = 0; x < bounds.getWidth(); x++) {
      Point<float> pixelLoc(x, y);
      constexpr int sampleGridSize = 4;
      float accum = 0.f;

      for (int sy = 0; sy < sampleGridSize; sy++) {
        for (int sx = 0; sx < sampleGridSize; sx++) {
          auto jitter = prng();
          float jx = float(jitter & 0xffff) / 0xffff;
          jitter >>= 16;
          float jy = float(jitter & 0xffff) / 0xffff;
          auto sampleLoc =
              pixelLoc + Point<float>(sx + jx, sy + jy) / sampleGridSize;
          auto point = layout.pointInfo(sampleLoc);
          accum +=
              point.valid ? (point.sample / double(gda.numSamples())) : 0.f;
        }
      }
      bits.setPixelColour(
          x, y, bg.contrasting(accum / juce::square<float>(sampleGridSize)));
    }
  }
}
