#include "WavePanel.h"
#include "GrainSynth.h"

class WavePanel::ImageBuilder {
public:
  struct Params {
    juce::Rectangle<int> bounds;
    juce::Colour background, highlight;
  };

  struct WavePlayback {
    GrainSequence::Point seq;
    juce::Range<int> samples;
  };

  struct WaveInfo {
    GrainWaveform::Ptr wave;
    GrainIndex::Ptr index;
    std::vector<WavePlayback> playing;
  };

  struct State {
    std::vector<GrainWaveform::Window> windows;
    std::unordered_map<GrainWaveform *, WaveInfo> waves;
    float widthInSamples{0.f};

    void ensureWidth(float minimum) {
      widthInSamples = std::max(widthInSamples, minimum);
    }

    void addPlayback(GrainWaveform &wave, GrainIndex &index,
                     const WavePlayback &playback) {
      auto &slot = waves[&wave];
      if (slot.wave == nullptr) {
        slot.wave = wave;
        slot.index = index;
      }
      jassert(slot.wave.get() == &wave);
      jassert(slot.index.get() == &index);
      slot.playing.push_back(playback);
    }

    void addSoundWindow(const GrainSound &sound) {
      ensureWidth(sound.params.common.maxGrainWidthSamples(*sound.index));
      windows.push_back(sound.params.common.window(*sound.index));
    }
  };

  ImageBuilder(const Params &params, const State &state)
      : params(params), state(state) {}
  ~ImageBuilder() {}

  std::unique_ptr<juce::Image> run() {
    if (params.bounds.isEmpty()) {
      return nullptr;
    }
    auto image = std::make_unique<juce::Image>(juce::Image::RGB, width(),
                                               height(), false);

    // Each sorted active grain gets a row in the backing image
    auto waves = collectWavesSortedByGrain();
    if (waves.size() > 0) {
      drawWaveCoverageMaps(waves, *image);
      drawWaveSources(waves, *image);
    } else {
      juce::Graphics g(*image);
      g.setColour(params.background);
      g.fillAll();
    }

    // Faint outline of all active window functions
    juce::Graphics g(*image);
    g.setColour(params.background.contrasting(1.f));
    g.setOpacity(0.2f);
    for (auto window : collectUniqueWaveformWindows()) {
      g.strokePath(pathForWindow(window), juce::PathStrokeType(2.f));
    }

    // Highlighted window functions
    g.setColour(params.highlight);
    g.setOpacity(0.4f);
    for (auto &window : state.windows) {
      g.strokePath(pathForWindow(window), juce::PathStrokeType(3.5f));
    }

    // Center column marker is topmost
    g.setColour(params.highlight);
    drawCenterColumn(g);

    return image;
  }

private:
  struct CoverageMap {
    std::vector<float> bins;

    void add(float start, float end, float amount) {
      int startFloor = std::floor(start), startCeil = std::ceil(start);
      int endFloor = std::floor(end), endCeil = std::ceil(end);
      if (startFloor != startCeil && startFloor >= 0 &&
          startFloor < bins.size()) {
        bins[startFloor] += amount * (startCeil - start);
      }
      for (int x = std::max<int>(0, startCeil);
           x < std::min<int>(bins.size(), endFloor); x++) {
        bins[x] += amount;
      }
      if (endFloor != endCeil && endFloor >= 0 && endFloor < bins.size()) {
        bins[endFloor] += amount * (end - endFloor);
      }
    }

    inline float peakValue() const noexcept {
      float result = 0.f;
      for (auto value : bins) {
        result = std::max(result, value);
      }
      return result;
    }
  };

  int width() const { return params.bounds.getWidth(); }
  int height() const { return params.bounds.getHeight(); }
  int centerColumn() const { return width() / 2; }
  float samplesPerColumn() const { return 2 * state.widthInSamples / width(); }

  void drawCoverageMap(juce::Image &image, const CoverageMap &map, float gain,
                       int top, int bottom) {
    jassert(map.bins.size() == width());
    juce::Image::BitmapData bits(image, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < map.bins.size(); x++) {
      for (int y = top; y < bottom; y++) {
        float t = (y - top) * M_PI / (bottom - top);
        auto c = params.background.contrasting(
            juce::jlimit(0.f, 1.f, sinf(t) * gain * map.bins[x]));
        bits.setPixelColour(x, y, c);
      }
    }
  }

  void drawCenterColumn(juce::Graphics &g) {
    auto thick = 2.5f, margin = 1.f;
    auto x = centerColumn(), h = height();
    g.drawLine(x, margin + thick, x, h - thick - margin, thick);
    g.fillEllipse(x - thick, margin, thick * 2, thick * 2);
    g.fillEllipse(x - thick, h - margin - thick * 2, thick * 2, thick * 2);
  }

  void drawWaveCoverageMaps(const std::vector<const WaveInfo *> &waves,
                            juce::Image &image) {
    int maxNumberOfCoverageMaps = height() / 2;
    std::vector<CoverageMap> maps(
        std::min<int>(waves.size(), maxNumberOfCoverageMaps));
    for (int i = 0; i < waves.size(); i++) {
      auto &map =
          maps[std::min<int>(maps.size() - 1, i * maps.size() / waves.size())];
      coverageForWavePlayback(*waves[i], map);
    }
    auto peakCoverage = 0.f;
    for (const auto &map : maps) {
      peakCoverage = std::max(peakCoverage, map.peakValue());
    }
    for (int i = 0; i < maps.size(); i++) {
      static constexpr float peakContrast = 1.5f;
      int top = i * height() / maps.size();
      int bottom = (i + 1) * height() / maps.size();
      drawCoverageMap(image, maps[i], peakContrast / peakCoverage, top, bottom);
    }
  }

  void drawWaveSources(const std::vector<const WaveInfo *> &waves,
                       juce::Image &image) {
    // The text fades out as it gets more crowded
    float height = image.getHeight() / float(waves.size());
    float opacity =
        juce::jlimit(0.f, 0.5f, juce::jmap(height, 2.f, 20.f, 0.f, 0.5f));
    if (opacity > 0.f) {
      juce::Graphics g(image);
      g.setColour(params.background.contrasting(1.f));
      g.setOpacity(opacity);
      for (int i = 0; i < waves.size(); i++) {
        unsigned grain = waves[i]->wave->key.grain;
        auto source = waves[i]->index->sources.pathForGrainSource(grain);
        g.drawFittedText(source, 0, height * i, image.getWidth(), height,
                         juce::Justification::left, 1);
      }
    }
  }

  std::vector<GrainWaveform::Window> collectUniqueWaveformWindows() const {
    std::vector<GrainWaveform::Window> result;
    for (auto &item : state.waves) {
      auto w = item.second.wave->key.window;
      if (std::find(state.windows.begin(), state.windows.end(), w) ==
              state.windows.end() &&
          std::find(result.begin(), result.end(), w) == result.end()) {
        result.push_back(w);
      }
    }
    return result;
  }

  std::vector<const WaveInfo *> collectWavesSortedByGrain() const {
    std::vector<const WaveInfo *> result;
    for (auto &item : state.waves) {
      result.push_back(&item.second);
    }
    std::sort(result.begin(), result.end(),
              [](const WaveInfo *a, const WaveInfo *b) {
                return a->wave->key.grain > b->wave->key.grain;
              });
    return result;
  }

  void coverageForWavePlayback(const WaveInfo &waveInfo, CoverageMap &map) {
    const auto &wave = *waveInfo.wave;
    map.bins.resize(width());
    for (const auto &playback : waveInfo.playing) {
      const auto &gains = playback.seq.gains;
      auto totalGain = std::accumulate(gains.begin(), gains.end(), 0.f);
      auto samples = playback.samples + wave.key.window.range().getStart();
      auto columnStart = std::max<float>(
          0.f, centerColumn() + samples.getStart() / samplesPerColumn());
      auto columnEnd = std::min<float>(map.bins.size() - 1,
                                       centerColumn() + samples.getEnd() /
                                                            samplesPerColumn());
      map.add(columnStart, columnEnd, totalGain);
    }
  }

  juce::Path pathForWindow(const GrainWaveform::Window &window) const {
    juce::Path path;
    const float top = height() * 0.1f;
    const float bottom = height() * 0.9f;
    auto peak = window.peakValue();
    auto spc = samplesPerColumn();
    for (int col = 0; col < width(); col++) {
      auto x = (col - centerColumn()) * spc;
      auto normalized = window.evaluate(x) / peak;
      auto y = bottom + normalized * (top - bottom);
      if (col == 0) {
        path.startNewSubPath(col, y);
      } else {
        path.lineTo(col, y);
      }
    }
    return path;
  }

  Params params;
  State state;
};

class WavePanel::RenderThread : public juce::Thread,
                                public juce::ChangeBroadcaster,
                                public GrainVoice::Listener {
public:
  RenderThread(GrainSynth &synth)
      : Thread("wave-image"), synth(synth),
        collector(std::make_unique<ImageBuilder::State>()) {}
  ~RenderThread() override {}

  void grainVoicePlaying(const GrainVoice &, const GrainSound &sound,
                         GrainWaveform &wave, const GrainSequence::Point &seq,
                         const juce::Range<int> &samples) override {
    if (samples.getLength() > 0) {
      std::lock_guard<std::mutex> guard(collectorMutex);
      collector->ensureWidth(
          sound.params.common.maxGrainWidthSamples(*sound.index));
      collector->addPlayback(wave, *sound.index, {seq, samples});
    }
  }

  void requestChange(const ImageBuilder::Params &req) {
    std::lock_guard<std::mutex> guard(requestMutex);
    request = req;
  }

  void drawLatest(juce::Graphics &g, juce::Rectangle<float> location) {
    std::lock_guard<std::mutex> guard(imageMutex);
    if (image != nullptr) {
      g.drawImage(*image, location);
    }
  }

  void run() override {
    static constexpr int approxFrameRateLimit = 30;
    static constexpr float imagePersistence = 0.6f;

    while (!threadShouldExit()) {
      auto nextImage = renderImage();
      if (nextImage != nullptr) {
        {
          std::lock_guard<std::mutex> guard(imageMutex);
          if (image != nullptr) {
            juce::Graphics g(*nextImage);
            g.setOpacity(imagePersistence);
            g.drawImage(*image, nextImage->getBounds().toFloat());
          }
          std::swap(image, nextImage);
        }
        sendChangeMessage();
      }
      wait(1000 / approxFrameRateLimit);
    }
  }

private:
  GrainSynth &synth;

  std::mutex requestMutex;
  ImageBuilder::Params request;

  std::mutex imageMutex;
  std::unique_ptr<juce::Image> image;

  std::mutex collectorMutex;
  std::unique_ptr<ImageBuilder::State> collector;

  std::unique_ptr<ImageBuilder::State> takeCollector() {
    auto result = std::make_unique<ImageBuilder::State>();
    std::lock_guard<std::mutex> guard(collectorMutex);
    std::swap(result, collector);
    return result;
  }

  ImageBuilder::Params latestRequest() {
    std::lock_guard<std::mutex> guard(requestMutex);
    return request;
  }

  std::unique_ptr<juce::Image> renderImage() {
    auto state = takeCollector();
    auto latestSound = synth.latestSound();
    if (latestSound != nullptr) {
      state->addSoundWindow(*latestSound);
    }
    return ImageBuilder(latestRequest(), *state).run();
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RenderThread)
};

WavePanel::WavePanel(RvvProcessor &p)
    : processor(p), thread(std::make_unique<RenderThread>(processor.synth)) {
  processor.synth.addListener(thread.get());
  thread->startThread();
  thread->addChangeListener(this);
}

WavePanel::~WavePanel() {
  processor.synth.removeListener(thread.get());
  thread->signalThreadShouldExit();
  thread->notify();
  thread->waitForThreadToExit(-1);
}

void WavePanel::resized() {
  thread->requestChange(ImageBuilder::Params{
      .bounds = getLocalBounds(),
      .background = findColour(juce::ResizableWindow::backgroundColourId),
      .highlight = findColour(juce::Slider::thumbColourId),
  });
}

void WavePanel::paint(juce::Graphics &g) {
  auto bounds = getLocalBounds().toFloat();
  thread->drawLatest(g, bounds);
}

void WavePanel::changeListenerCallback(juce::ChangeBroadcaster *) {
  const juce::MessageManagerLock mmlock;
  repaint();
}
