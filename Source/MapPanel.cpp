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
      : bounds(bounds), gda(gda) {
    pitchRange = gda.pitchRange();
    if (pitchRange.isEmpty()) {
      logPitchRatio = 0.f;
    } else {
      float pitchRatio = pitchRange.getEnd() / pitchRange.getStart();
      logPitchRatio = log(pitchRatio);
    }
  }

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
      result.bin = gda.closestBinForPitch(pitchRange.getStart() *
                                          exp(relX * logPitchRatio));

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
  juce::Range<float> pitchRange;
  float logPitchRatio;
};

static std::unique_ptr<juce::Image> renderImage(const MapImage::Request &req,
                                                GrainData &grainData) {
  GrainData::Accessor gda(grainData);
  auto width = req.bounds.getWidth(), height = req.bounds.getHeight();
  if (!(width > 0 && height > 0 && gda.numSamples() && gda.numBins() &&
        gda.numGrains())) {
    return nullptr;
  }

  auto image = std::make_unique<Image>(Image::RGB, width, height, false);
  auto layout = MapLayout(req.bounds.toFloat(), gda);
  Image::BitmapData bits(*image, juce::Image::BitmapData::writeOnly);
  std::mt19937 prng;
  constexpr int sampleGridSize = 4;

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      Point<float> pixelLoc(x, y);
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

      accum /= juce::square<float>(sampleGridSize);
      auto color = req.background.contrasting(accum);
      bits.setPixelColour(x, y, color);
    }
  }
  return image;
}

MapPanel::MapPanel(AudioProcessor &audioProcessor,
                   juce::TimeSliceThread &thread)
    : audioProcessor(audioProcessor),
      mapImage(audioProcessor.grainData, thread) {
  audioProcessor.grainData.referToStatusOutput(grainDataStatus);
  grainDataStatus.addListener(this);
  mapImage.addChangeListener(this);
}

MapPanel::~MapPanel() { mapImage.removeChangeListener(this); }

void MapPanel::paint(juce::Graphics &g) {
  mapImage.drawLatestImage(g, getLocalBounds().toFloat());
}

void MapPanel::resized() {
  // Start working on a new image but keep the old one
  requestNewImage();
}

void MapPanel::valueChanged(juce::Value &) {
  // Immediately discard image when the data source is changed
  mapImage.discardStoredImage();
  requestNewImage();
}

void MapPanel::requestNewImage() {
  mapImage.requestChange(MapImage::Request{
      .bounds = getLocalBounds(),
      .background = findColour(juce::ResizableWindow::backgroundColourId),
  });
}

void MapPanel::changeListenerCallback(juce::ChangeBroadcaster *) { repaint(); }

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

void MapImage::Request::usePreviewResolution() {
  constexpr int maxDimension = 120;
  int dimension = juce::jmax(bounds.getWidth(), bounds.getHeight());
  if (dimension > maxDimension) {
    bounds = (bounds.toFloat() * maxDimension / dimension).toNearestInt();
  }
}

MapImage::MapImage(GrainData &grainData, juce::TimeSliceThread &thread)
    : thread(thread), grainData(grainData) {}

MapImage::~MapImage() { thread.removeTimeSliceClient(this); }

void MapImage::requestChange(const Request &req) {
  juce::ScopedLock sl(mutex);
  nextRequest = req;
  thread.addTimeSliceClient(this);
}

void MapImage::discardStoredImage() {
  juce::ScopedLock sl(mutex);
  lastRequest = Request{};
  image = nullptr;
}

void MapImage::drawLatestImage(juce::Graphics &g,
                               juce::Rectangle<float> location) {
  juce::ScopedLock sl(mutex);
  if (image) {
    g.drawImage(*image, location);
  }
}

int MapImage::useTimeSlice() {
  Request req;
  {
    juce::ScopedLock sl(mutex);
    req = nextRequest;
    if (image && lastRequest == req) {
      // Idle
      return -1;
    }
    if (!image) {
      req.usePreviewResolution();
    }
  }
  auto newImage = renderImage(req, grainData);
  {
    juce::ScopedLock sl(mutex);
    lastRequest = req;
    std::swap(newImage, image);
  }
  {
    const juce::MessageManagerLock mmlock;
    sendChangeMessage();
  }
  // Re-check for requests that may have arrived during render
  return 0;
}
