#include "MapPanel.h"
#include "GrainData.h"
#include <random>

class MapPanel::Layout {
public:
  Layout(const juce::Rectangle<float> &bounds, const GrainIndex &index)
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
    juce::uint64 sample;
  };

  inline PointInfo pointInfo(const juce::Point<float> &p) {
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
  juce::Rectangle<float> bounds;
  const GrainIndex &index;
  juce::Range<float> pitchRange;
  float logPitchRatio;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Layout)
};

class MapPanel::ImageRender : private juce::ThreadPoolJob,
                              public juce::ChangeBroadcaster {
public:
  ImageRender(juce::ThreadPool &pool)
      : ThreadPoolJob("map_image_render"), pool(pool) {}

  ~ImageRender() override { pool.waitForJobToFinish(this, -1); }

  struct Request {
    GrainIndex::Ptr index;
    juce::Rectangle<int> bounds;
    juce::Colour background;

    bool operator==(const Request &r) {
      return index == r.index && bounds == r.bounds &&
             background == r.background;
    }

    void usePreviewResolution() {
      constexpr int maxDimension = 120;
      int dimension = juce::jmax(bounds.getWidth(), bounds.getHeight());
      if (dimension > maxDimension) {
        bounds = (bounds.toFloat() * maxDimension / dimension).toNearestInt();
      }
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
      pool.addJob(this, false);
    }
  }

  void drawLatest(juce::Graphics &g, juce::Rectangle<float> location) {
    std::lock_guard<std::mutex> guard(lock);
    if (image) {
      g.drawImage(*image, location);
    }
  }

private:
  juce::ThreadPool &pool;
  std::mutex lock;
  std::unique_ptr<juce::Image> image;
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
      if (!image || lastRequest.index != req.index) {
        req.usePreviewResolution();
      }
    }
    auto newImage = render(req);
    {
      std::lock_guard<std::mutex> guard(lock);
      lastRequest = req;
      std::swap(newImage, image);
    }
    sendChangeMessage();
    // Re-check for requests that may have arrived during render
    return JobStatus::jobNeedsRunningAgain;
  }

  static std::unique_ptr<juce::Image> render(const Request &req) {
    constexpr double hueSpread = 18.;
    constexpr float lightnessExponent = 2.5f;
    constexpr float foregroundContrast = 0.7f;
    constexpr int sampleGridSize = 4;

    auto bg = req.background;
    auto fg = bg.contrasting(foregroundContrast);
    float bgH, bgS, bgL, fgH, fgS, fgL;
    bg.getHSL(bgH, bgS, bgL);
    fg.getHSL(fgH, fgS, fgL);

    auto width = req.bounds.getWidth(), height = req.bounds.getHeight();
    auto image = std::make_unique<juce::Image>(
        juce::Image::RGB, std::max(1, width), std::max(1, height), false);

    if (req.index && req.index->isValid()) {
      Layout layout(req.bounds.toFloat(), *req.index);
      double numSamples = double(req.index->numSamples);
      juce::Image::BitmapData bits(*image, juce::Image::BitmapData::writeOnly);
      std::mt19937 jitterPrng;

      for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
          juce::Point<float> pixelLoc(x, y);
          double accum = 0.;

          for (int sy = 0; sy < sampleGridSize; sy++) {
            for (int sx = 0; sx < sampleGridSize; sx++) {
              auto jitter = jitterPrng();
              float jx = float(jitter & 0xffff) / float(0xffff);
              float jy = float((jitter >> 16) & 0xffff) / float(0xffff);
              auto sub = juce::Point<float>(sx + jx, sy + jy) / sampleGridSize;
              auto point = layout.pointInfo(pixelLoc + sub);
              accum += point.valid ? (point.sample / numSamples) : 0.f;
            }
          }
          accum /= juce::square<float>(sampleGridSize);

          auto cmH = float(fmod(bgH + hueSpread * accum, 1.0));
          auto cmS = bgS + (fgS - bgS) * float(accum);
          auto cmL = bgL + (fgL - bgL) * powf(float(accum), lightnessExponent);
          auto colormap = juce::Colour::fromHSL(cmH, cmS, cmL, 1.0f);

          bits.setPixelColour(x, y, colormap);
        }
      }
    } else {
      image->clear(req.bounds, bg);
    }
    return image;
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImageRender)
};

MapPanel::MapPanel(RvvProcessor &processor)
    : processor(processor),
      image(std::make_unique<ImageRender>(processor.generalPurposeThreads)) {
  processor.grainData.referToStatusOutput(grainDataStatus);
  grainDataStatus.addListener(this);
  image->addChangeListener(this);
}

MapPanel::~MapPanel() { image->removeChangeListener(this); }

void MapPanel::paint(juce::Graphics &g) {
  image->drawLatest(g, getLocalBounds().toFloat());
}

void MapPanel::resized() { requestNewImage(); }

void MapPanel::valueChanged(juce::Value &) { requestNewImage(); }

void MapPanel::requestNewImage() {
  image->requestChange(ImageRender::Request{
      .index = processor.grainData.getIndex(),
      .bounds = getLocalBounds(),
      .background = findColour(juce::ResizableWindow::backgroundColourId),
  });
}

void MapPanel::changeListenerCallback(juce::ChangeBroadcaster *) {
  const juce::MessageManagerLock mmlock;
  repaint();
}
