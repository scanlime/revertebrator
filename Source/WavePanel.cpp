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
    std::vector<WavePlayback> playing;
  };

  struct State {
    std::vector<GrainWaveform::Window> windows;
    std::unordered_map<GrainWaveform *, WaveInfo> waves;
    float widthInSamples{0.f};

    void ensureWidth(float minimum) {
      widthInSamples = std::max(widthInSamples, minimum);
    }

    void addPlayback(GrainWaveform &wave, const WavePlayback &playback) {
      auto &slot = waves[&wave];
      if (slot.wave == nullptr) {
        slot.wave = wave;
      }
      jassert(slot.wave.get() == &wave);
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
      for (int i = 0; i < waves.size(); i++) {
        drawWaveRow(*image, *waves[i], i * height() / waves.size(),
                    (i + 1) * height() / waves.size());
      }
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
  int width() const { return params.bounds.getWidth(); }
  int height() const { return params.bounds.getHeight(); }
  int centerColumn() const { return width() / 2; }
  float samplesPerColumn() const { return 2 * state.widthInSamples / width(); }

  void drawWaveRow(juce::Image &image, const WaveInfo &waveInfo, int top,
                   int bottom) {
    const auto &wave = *waveInfo.wave;

// xx automatic gain control where

    // Make a big gain coverage map for this waveform, with sample resolution
    std::vector<float> coverage(wave.getNumSamples());
    for (const auto &playback : wave.playing) {
      const auto &gains = playback.seq.gains;
      auto totalGain = std::accumulate(gains.begin(), gains.end(), 0.f);
      auto begin = std::max(0, playback.samples.getBegin());
      auto end = std::min(coverage.size()-1, playback.samples.getEnd());
      for (auto x = begin; x < end; x++) {
        coverage[x] += totalGain;
      }
    }

    // Downsample to the image's width

}

    std;

    juce::Image::BitmapData bits(image, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < width(); x++) {
      for (int y = top; y < bottom; y++) {
        auto c = params.background.contrasting(0.5f);
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
      collector->addPlayback(wave, {seq, samples});
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
