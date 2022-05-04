#include "MapPanel.h"
#include "GrainData.h"
#include <random>

using juce::Colour;
using juce::Image;
using juce::Point;
using juce::Rectangle;
using juce::uint64;

class MapPanel::Layout {
public:
  Layout(const Rectangle<float> &bounds, const GrainIndex &index)
      : bounds(bounds), index(index) {
    pitchRange = index.pitchRange();
    if (pitchRange.isEmpty()) {
      logPitchRatio = 0.f;
    } else {
      logPitchRatio = log(pitchRange.getEnd() / pitchRange.getStart());
    }
  }

  struct PointInfo {
    bool valid;
    unsigned bin, grain;
    uint64 sample;
  };

  inline PointInfo pointInfo(const Point<float> &p) {
    if (bounds.contains(p)) {
      auto result = PointInfo{.valid = true};

      // X axis: logarithmic pitch
      float relX = (p.x - bounds.getX()) / (bounds.getWidth() - 1);
      result.bin = index.closestBinForPitch(pitchRange.getStart() *
                                            exp(relX * logPitchRatio));

      // Y axis: linear grain selector, variable resolution
      auto gr = index.grainsForBin(result.bin);
      float relY = (p.y - bounds.getY()) / bounds.getHeight();
      result.grain = gr.clipValue(gr.getStart() + gr.getLength() * relY);
      result.sample = index.grainX[result.grain];
      return result;
    } else {
      return PointInfo{false};
    }
  }

private:
  Rectangle<float> bounds;
  const GrainIndex &index;
  juce::Range<float> pitchRange;
  float logPitchRatio;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Layout)
};

class MapPanel::ImageRender : private juce::ThreadPoolJob,
                              public juce::ChangeBroadcaster {
public:
  ImageRender(AudioProcessor &audioProcessor)
      : ThreadPoolJob("map_image_render"), audioProcessor(audioProcessor) {}

  ~ImageRender() override {
    audioProcessor.generalPurposeThreads.waitForJobToFinish(this, -1);
  }

  struct Request {
    juce::Rectangle<int> bounds;
    juce::Colour background;

    void usePreviewResolution() {
      constexpr int maxDimension = 120;
      int dimension = juce::jmax(bounds.getWidth(), bounds.getHeight());
      if (dimension > maxDimension) {
        bounds = (bounds.toFloat() * maxDimension / dimension).toNearestInt();
      }
    }

    bool operator==(const Request &r) {
      return bounds == r.bounds && background == r.background;
    }
  };

  void requestChange(const Request &req) {
    bool shouldAddJob;
    {
      std::lock_guard<std::mutex> guard(lock);
      nextRequest = req;
      shouldAddJob = !isPending;
      isPending = true;
    }
    if (shouldAddJob) {
      audioProcessor.generalPurposeThreads.addJob(this, false);
    }
  }

  void discardStored() {
    std::lock_guard<std::mutex> guard(lock);
    lastRequest = Request{};
    image = nullptr;
  }

  void drawLatest(juce::Graphics &g, juce::Rectangle<float> location) {
    std::lock_guard<std::mutex> guard(lock);
    if (image) {
      g.drawImage(*image, location);
    }
  }

private:
  AudioProcessor &audioProcessor;
  std::mutex lock;
  std::unique_ptr<Image> image;
  Request lastRequest, nextRequest;
  bool isPending{false};

  JobStatus runJob() override {
    Request req;
    {
      std::lock_guard<std::mutex> guard(lock);
      req = nextRequest;
      if (image && lastRequest == req) {
        isPending = false;
        return JobStatus::jobHasFinished;
      }
      if (!image) {
        req.usePreviewResolution();
      }
    }
    auto index = audioProcessor.grainData.getIndex();
    auto newImage = index ? renderImage(req, *index) : nullptr;
    {
      std::lock_guard<std::mutex> guard(lock);
      lastRequest = req;
      std::swap(newImage, image);
    }
    sendChangeMessage();
    // Re-check for requests that may have arrived during render
    return JobStatus::jobNeedsRunningAgain;
  }

  static std::unique_ptr<Image> renderImage(const Request &req,
                                            const GrainIndex &index) {
    constexpr double hueSpread = 18.;
    constexpr float lightnessExponent = 2.5f;
    constexpr float foregroundContrast = 0.7f;
    constexpr int sampleGridSize = 4;

    auto width = req.bounds.getWidth(), height = req.bounds.getHeight();
    if (!(width > 0 && height > 0 && index.numSamples > 0 &&
          index.numBins > 0 && index.numGrains > 0)) {
      return nullptr;
    }

    auto bg = req.background;
    auto fg = bg.contrasting(foregroundContrast);
    float bgH, bgS, bgL, fgH, fgS, fgL;
    bg.getHSL(bgH, bgS, bgL);
    fg.getHSL(fgH, fgS, fgL);

    auto image = std::make_unique<Image>(Image::RGB, width, height, false);
    Layout layout(req.bounds.toFloat(), index);
    Image::BitmapData bits(*image, Image::BitmapData::writeOnly);
    std::mt19937 prng;

    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        Point<float> pixelLoc(x, y);
        double accum = 0.;

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
                point.valid ? (point.sample / double(index.numSamples)) : 0.f;
          }
        }
        accum /= juce::square<float>(sampleGridSize);

        auto cmH = float(fmod(bgH + hueSpread * accum, 1.0));
        auto cmS = bgS + (fgS - bgS) * float(accum);
        auto cmL = bgL + (fgL - bgL) * powf(float(accum), lightnessExponent);
        auto colormap = Colour::fromHSL(cmH, cmS, cmL, 1.0f);

        bits.setPixelColour(x, y, colormap);
      }
    }
    return image;
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImageRender)
};

MapPanel::MapPanel(AudioProcessor &audioProcessor)
    : audioProcessor(audioProcessor),
      image(std::make_unique<ImageRender>(audioProcessor)) {
  audioProcessor.grainData.referToStatusOutput(grainDataStatus);
  grainDataStatus.addListener(this);
  image->addChangeListener(this);
}

MapPanel::~MapPanel() { image->removeChangeListener(this); }

void MapPanel::paint(juce::Graphics &g) {
  image->drawLatest(g, getLocalBounds().toFloat());
}

void MapPanel::resized() {
  // Start working on a new image but keep the old one
  requestNewImage();
}

void MapPanel::valueChanged(juce::Value &) {
  // Immediately discard image when the data source is changed
  image->discardStored();
  requestNewImage();
}

void MapPanel::requestNewImage() {
  image->requestChange(ImageRender::Request{
      .bounds = getLocalBounds(),
      .background = findColour(juce::ResizableWindow::backgroundColourId),
  });
}

void MapPanel::changeListenerCallback(juce::ChangeBroadcaster *) {
  const juce::MessageManagerLock mmlock;
  repaint();
}

void MapPanel::mouseMove(const juce::MouseEvent &event) {
  GrainIndex::Ptr index = audioProcessor.grainData.getIndex();
  if (index) {
    Layout layout(getLocalBounds().toFloat(), *index);
    auto point = layout.pointInfo(event.getPosition().toFloat());
    if (point.valid) {
      printf("grain %d bin %d, %f Hz\n", point.grain, point.bin,
             index->binF0[point.bin]);
    }
  }
}
