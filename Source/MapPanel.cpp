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
    float pitch, sel;
    unsigned bin, grain;
    juce::uint64 sample;
  };

  inline PointInfo pointInfo(const juce::Point<float> &p) const {
    if (bounds.contains(p)) {
      auto result = PointInfo{.valid = true};

      // X axis: logarithmic pitch
      float relX = (p.x - bounds.getX()) / (bounds.getWidth() - 1);
      result.pitch = pitchRange.getStart() * exp(relX * logPitchRatio);
      result.bin = index.closestBinForPitch(result.pitch);

      // Y axis: linear grain selector, variable resolution
      auto gr = index.grainsForBin(result.bin);
      float relY = (p.y - bounds.getY()) / bounds.getHeight();
      result.sel = 1.f - relY;
      result.grain = gr.clipValue(gr.getStart() + gr.getLength() * result.sel);
      result.sample = index.grainX[result.grain];
      return result;
    } else {
      return PointInfo{false};
    }
  }

  inline float xCoordForBinCenter(unsigned bin) const {
    if (bin < index.numBins()) {
      auto hz = index.binF0[bin];
      auto relX = log(hz / pitchRange.getStart()) / logPitchRatio;
      return bounds.getX() + relX * (bounds.getWidth() - 1);
    } else {
      return bounds.getRight();
    }
  }

  inline float xCoordLeftOfBin(unsigned bin) const {
    if (bin > 0) {
      return (xCoordForBinCenter(bin - 1) + xCoordForBinCenter(bin)) / 2.f;
    } else {
      return bounds.getX();
    }
  }

  inline float xCoordRightOfBin(unsigned bin) const {
    if (bin < (index.numBins() - 1)) {
      return (xCoordForBinCenter(bin + 1) + xCoordForBinCenter(bin)) / 2.f;
    } else {
      return bounds.getRight();
    }
  }

  inline float yCoordForGrain(const juce::Range<unsigned> &grainsForBin,
                              unsigned grain) const {
    if (grain < grainsForBin.getStart()) {
      return bounds.getHeight();
    } else if (grain >= grainsForBin.getEnd()) {
      return 0;
    } else {
      auto relY = 1.f - float(grain - grainsForBin.getStart()) /
                            grainsForBin.getLength();
      return bounds.getY() + relY * (bounds.getHeight() - 1);
    }
    jassert(grainsForBin.contains(grain));
  }

  inline juce::Rectangle<float> grainRectangle(unsigned grain) const {
    jassert(grain < index.numGrains());
    unsigned bin = index.binForGrain(grain);
    auto grains = index.grainsForBin(bin);
    return juce::Rectangle<float>::leftTopRightBottom(
        xCoordLeftOfBin(bin), yCoordForGrain(grains, grain + 1),
        xCoordRightOfBin(bin), yCoordForGrain(grains, grain));
  }

  inline juce::Rectangle<float> expandedGrainRectangle(unsigned grain) const {
    return grainRectangle(grain).expanded(2.5f);
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
      : ThreadPoolJob("map-image"), pool(pool) {}

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
    jassert(req.index);
    jassert(req.index->isValid());
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
    if (image != nullptr) {
      g.drawImage(*image, location);
    }
  }

  bool isEmpty() {
    std::lock_guard<std::mutex> guard(lock);
    return image == nullptr;
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
    jassert(req.index != nullptr);
    jassert(req.index->isValid());

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
    if (width < 1 || height < 1) {
      return nullptr;
    }
    auto image =
        std::make_unique<juce::Image>(juce::Image::RGB, width, height, false);
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
    return image;
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImageRender)
};

class MapPanel::LiveOverlay : private GrainWaveformCache::Listener,
                              private GrainVoice::Listener {
public:
  LiveOverlay(GrainIndex &index, GrainSynth &synth)
      : index(index), synth(synth) {
    index.cache.addListener(this);
    synth.addListener(this);
  }

  ~LiveOverlay() {
    index->cache.removeListener(this);
    synth.removeListener(this);
  }

  GrainIndex::Ptr index;
  GrainSynth &synth;

  struct Colors {
    juce::Colour loading, visited, playing, outline;
  };

  void paint(juce::Graphics &g, const juce::Rectangle<float> &bounds,
             const Colors &colors) {
    Layout layout(bounds, *index);
    juce::SortedSet<unsigned> visited, playing, startLoading, stopLoading;
    {
      std::lock_guard<std::mutex> guard(collector.mutex);
      visited.swapWith(collector.visited);
      playing.swapWith(collector.playing);
      startLoading.swapWith(collector.startLoading);
      stopLoading.swapWith(collector.stopLoading);
    }

    loading.addSet(startLoading);
    loading.removeValuesIn(stopLoading);

    drawGrainSet(g, layout, loading, colors.loading);
    drawGrainSet(g, layout, playing, colors.playing, colors.outline);
    drawGrainSet(g, layout, visited, colors.visited);
  }

private:
  juce::SortedSet<unsigned> loading;
  struct {
    std::mutex mutex;
    juce::SortedSet<unsigned> visited, playing, startLoading, stopLoading;
  } collector;

  static void drawGrainSet(juce::Graphics &g, const Layout &layout,
                           juce::SortedSet<unsigned> &grains, juce::Colour fill,
                           juce::Colour outline = {}) {
    juce::RectangleList<float> rects;
    rects.ensureStorageAllocated(grains.size());
    for (auto grain : grains) {
      rects.addWithoutMerging(layout.expandedGrainRectangle(grain));
    }
    if (!outline.isTransparent()) {
      g.setColour(outline);
      g.strokePath(rects.toPath(), juce::PathStrokeType(1.5f));
    }
    g.setColour(fill);
    g.fillRectList(rects);
  }

  void grainWaveformStored(const GrainWaveform::Key &key) override {
    jassert(key.grain < index->numGrains());
    std::lock_guard<std::mutex> guard(collector.mutex);
    collector.stopLoading.add(key.grain);
  }

  void grainWaveformLookup(const GrainWaveform::Key &key,
                           bool dataFound) override {
    jassert(key.grain < index->numGrains());
    std::lock_guard<std::mutex> guard(collector.mutex);
    if (dataFound) {
      collector.visited.add(key.grain);
    } else {
      collector.startLoading.add(key.grain);
    }
  }

  void grainWaveformExpired(const GrainWaveform::Key &key) override {
    jassert(key.grain < index->numGrains());
    std::lock_guard<std::mutex> guard(collector.mutex);
    collector.stopLoading.add(key.grain);
  }

  void grainVoicePlaying(const GrainVoice &, const GrainSound &sound,
                         GrainWaveform &wave, const GrainSequence::Point &,
                         const juce::Range<int> &) override {
    if (index == sound.index) {
      jassert(wave.key.grain < index->numGrains());
      std::lock_guard<std::mutex> guard(collector.mutex);
      collector.playing.add(wave.key.grain);
    }
  }
};

MapPanel::MapPanel(RvvProcessor &processor)
    : processor(processor),
      image(std::make_unique<ImageRender>(processor.generalPurposeThreads)) {
  processor.grainData.referToStatusOutput(grainDataStatus);
  image->addChangeListener(this);
  grainDataStatus.addListener(this);
  valueChanged(grainDataStatus);
}

MapPanel::~MapPanel() { image->removeChangeListener(this); }

void MapPanel::paint(juce::Graphics &g) {
  auto bounds = getLocalBounds().toFloat();
  image->drawLatest(g, bounds);
  if (live) {
    auto background = findColour(juce::ResizableWindow::backgroundColourId);
    auto highlight = findColour(juce::Slider::thumbColourId);
    auto reddish = highlight.withRotatedHue(0.5f).interpolatedWith(
        juce::Colours::red, 0.5f);
    live->paint(g, bounds,
                LiveOverlay::Colors{
                    .loading = reddish.withAlpha(0.4f),
                    .visited = background.contrasting().withAlpha(0.6f),
                    .playing = highlight,
                    .outline = background,
                });
  }
}

void MapPanel::valueChanged(juce::Value &) {
  auto index = processor.grainData.getIndex();
  if (index && index->isValid()) {
    requestNewImage(*index);
    live = std::make_unique<LiveOverlay>(*index, processor.synth);
  } else {
    live = nullptr;
  }
}

void MapPanel::resized() {
  auto index = processor.grainData.getIndex();
  if (index && index->isValid()) {
    requestNewImage(*index);
  }
}

void MapPanel::timerCallback() { repaint(); }

void MapPanel::mouseDown(const juce::MouseEvent &e) {
  updateGrainUnderMouse(e, true);
}

void MapPanel::mouseUp(const juce::MouseEvent &e) {
  updateGrainUnderMouse(e, false);
}

void MapPanel::mouseEnter(const juce::MouseEvent &e) {
  updateGrainUnderMouse(e, false);
}

void MapPanel::mouseExit(const juce::MouseEvent &e) {
  updateGrainUnderMouse(e, false);
};

void MapPanel::mouseMove(const juce::MouseEvent &e) {
  updateGrainUnderMouse(e, false);
};

void MapPanel::mouseDrag(const juce::MouseEvent &e) {
  updateGrainUnderMouse(e, true);
}

void MapPanel::requestNewImage(GrainIndex &index) {
  image->requestChange(ImageRender::Request{
      .index = index,
      .bounds = getLocalBounds(),
      .background = findColour(juce::ResizableWindow::backgroundColourId),
  });
}

void MapPanel::updateGrainUnderMouse(const juce::MouseEvent &e, bool isDown) {
  float pitch = 0.f, sel = 0.f, velocity = 0.f;
  auto index = processor.grainData.getIndex();
  auto sound = processor.synth.latestSound();
  if (isDown && index != nullptr && index->isValid() && sound != nullptr) {
    Layout layout(getLocalBounds().toFloat(), *index);
    auto point = layout.pointInfo(e.getEventRelativeTo(this).position);
    if (point.valid) {
      pitch = point.pitch * sound->params.common.speedWarp;
      sel = point.sel;
      velocity = 0.7f;
    }
  }
  processor.touchEvent({
      .sourceId = e.source.getIndex(),
      .grain = {pitch, sel, velocity},
  });
}

void MapPanel::changeListenerCallback(juce::ChangeBroadcaster *) {
  const juce::MessageManagerLock mmlock;
  repaint();
  if (image->isEmpty() && isTimerRunning()) {
    stopTimer();
  } else if (!image->isEmpty() && !isTimerRunning()) {
    startTimerHz(15);
  }
}
