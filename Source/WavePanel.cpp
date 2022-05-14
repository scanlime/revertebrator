#include "WavePanel.h"
#include <unordered_map>

class WavePanel::ImageRender : public juce::Thread,
                               public juce::ChangeBroadcaster,
                               public GrainVoice::Listener {
public:
  ImageRender(GrainSynth &synth) : Thread("wave-image"), synth(synth) {}
  ~ImageRender() override {}

  struct Request {
    juce::Rectangle<int> bounds;
    juce::Colour background, highlight;
  };

  void grainVoicePlaying(const GrainVoice &, const GrainSound &sound,
                         GrainWaveform &wave, const GrainSequence::Point &seq,
                         int sampleNum, int sampleCount) override {
    auto maxWidth = sound.maxGrainWidthSamples();
    std::lock_guard<std::mutex> guard(collectorMutex);
    maxWidthForNextCollector = std::max(maxWidthForNextCollector, maxWidth);
    if (collector) {
      collector->playing(wave, seq, sampleNum, sampleCount);
    }
  }

  void visualizeSoundSettings(const GrainSound &sound) {
    auto maxWidth = sound.maxGrainWidthSamples();
    std::lock_guard<std::mutex> guard(collectorMutex);
    maxWidthForNextCollector = std::max(maxWidthForNextCollector, maxWidth);
    if (collector) {
      collector->windows.push_back(sound.getWindow());
    }
  }

  void requestChange(const Request &req) {
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
    static constexpr int approxFrameRateLimit = 90;
    while (!threadShouldExit()) {
      auto nextImage = renderImage();
      if (nextImage != nullptr) {
        {
          std::lock_guard<std::mutex> guard(imageMutex);
          std::swap(image, nextImage);
        }
        sendChangeMessage();
      }
      wait(1000 / approxFrameRateLimit);
    }
  }

private:
  class CoverageFilter {
  public:
    static constexpr int height = 5;
    static constexpr int border = 1;
    static constexpr float persistence = 0.75f;
    static constexpr float gainAdjustmentRate = 1e-2;

    CoverageFilter() {}

    void add(const std::vector<float> &coverage) {
      if (coverage.size() != accumulator.size()) {
        accumulator.clear();
        accumulator.resize(coverage.size());
      }
      for (int i = 0; i < accumulator.size(); i++) {
        accumulator[i] += coverage[i];
      }
    }

    juce::Image &renderImage() {
      if (image.isNull() || image.getWidth() != accumulator.size()) {
        image = juce::Image(juce::Image::SingleChannel,
                            std::max<int>(1, accumulator.size()), height, true);
      }
      juce::Image::BitmapData bits(image, juce::Image::BitmapData::writeOnly);

      float peak = 0.f;
      for (auto value : accumulator) {
        peak = std::max(peak, value);
      }
      float targetGain = std::min(1e6f, peak > 0.f ? 255.f / peak : 0.f);
      gain += (targetGain - gain) * gainAdjustmentRate;
      for (int x = 0; x < accumulator.size(); x++) {
        juce::uint8 alpha =
            juce::jlimit<int>(0, 255, std::round(accumulator[x] * gain));
        for (int y = border; y < height - border; y++) {
          bits.setPixelColour(x, y, juce::Colour().withAlpha(alpha));
        }
      }
      for (auto &value : accumulator) {
        value *= persistence;
      }
      return image;
    }

  private:
    std::vector<float> accumulator;
    float gain{0.};
    juce::Image image;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CoverageFilter)
  };

  class Collector {
  public:
    struct WaveInfo {
      GrainWaveform::Ptr wave;
      std::vector<float> coverage;

      void addCoverage(float start, float end, float amount) {
        int startFloor = std::floor(start), startCeil = std::ceil(start);
        int endFloor = std::floor(end), endCeil = std::ceil(end);
        if (startFloor != startCeil && startFloor >= 0 &&
            startFloor < coverage.size()) {
          coverage[startFloor] += amount * (startCeil - start);
        }
        for (int x = std::max<int>(0, startCeil);
             x < std::min<int>(coverage.size(), endFloor); x++) {
          coverage[x] += amount;
        }
        if (endFloor != endCeil && endFloor >= 0 &&
            endFloor < coverage.size()) {
          coverage[endFloor] += amount * (end - endFloor);
        }
      }
    };

    std::vector<GrainWaveform::Window> windows;
    std::unordered_map<GrainWaveform *, WaveInfo> waves;
    int numColumns, samplesPerTimeStep;
    float samplesPerColumn;

    Collector(int numColumns, float maxGrainWidthSamples)
        : numColumns(numColumns),
          samplesPerColumn(2.f * maxGrainWidthSamples / numColumns) {}

    int centerColumn() const { return numColumns / 2; }

    void drawCenterColumn(juce::Graphics &g, int height) {
      auto thick = 2.5f;
      auto margin = 1.f;
      auto x = centerColumn();
      g.drawLine(x, margin + thick, x, height - thick - margin, thick);
      g.fillEllipse(x - thick, margin, thick * 2, thick * 2);
      g.fillEllipse(x - thick, height - margin - thick * 2, thick * 2,
                    thick * 2);
    }

    void playing(GrainWaveform &wave, const GrainSequence::Point &seq,
                 int sampleNum, int sampleCount) {
      if (sampleCount < 1) {
        // This is a stand-in for a grain that couldn't be loaded in time
        return;
      }
      // Track the playing audio per-waveform
      auto &waveInfo = waves[&wave];
      if (waveInfo.wave == nullptr) {
        waveInfo.wave = &wave;
        waveInfo.coverage.resize(numColumns);
      }
      jassert(waveInfo.wave == &wave);
      jassert(waveInfo.coverage.size() == numColumns);

      auto sampleStart = wave.key.window.range().getStart() + sampleNum;
      auto sampleEnd = sampleStart + sampleCount;
      waveInfo.addCoverage(
          std::max<float>(0, centerColumn() + sampleStart / samplesPerColumn),
          std::min<float>(numColumns,
                          centerColumn() + sampleEnd / samplesPerColumn),
          seq.gain);
    }

    juce::Path pathForWindow(const GrainWaveform::Window &window,
                             float height) const {
      const float top = height * 0.1f;
      const float bottom = height * 0.9f;
      auto peak = window.peakValue();
      juce::Path path;
      for (int col = 0; col < numColumns; col++) {
        auto x = (col - centerColumn()) * samplesPerColumn;
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

    std::vector<GrainWaveform::Window> collectUniqueWaveformWindows() const {
      std::vector<GrainWaveform::Window> result;
      for (auto &item : waves) {
        auto w = item.second.wave->key.window;
        if (std::find(windows.begin(), windows.end(), w) == windows.end() &&
            std::find(result.begin(), result.end(), w) == result.end()) {
          result.push_back(w);
        }
      }
      return result;
    }

    std::unique_ptr<juce::Image> renderImage(const Request &req,
                                             CoverageFilter &coverage) {
      auto height = req.bounds.getHeight();
      if (height < 1) {
        return nullptr;
      }
      auto image = std::make_unique<juce::Image>(juce::Image::RGB, numColumns,
                                                 height, false);

      juce::Graphics g(*image);
      g.fillAll(req.background);
      g.setColour(req.background.contrasting(1.f));

      // Coverage (antialiased playback positions) for active waveforms
      for (auto &item : waves) {
        coverage.add(item.second.coverage);
      }
      g.setOpacity(0.7f);
      g.drawImage(coverage.renderImage(), 0, 0, numColumns, height, 0, 0,
                  numColumns, coverage.height, true);

      // Faint outline of all active window functions
      for (auto window : collectUniqueWaveformWindows()) {
        g.setOpacity(0.2f);
        g.strokePath(pathForWindow(window, height), juce::PathStrokeType(2.f));
      }

      // Highlighted window functions
      g.setColour(req.highlight);
      for (auto &window : windows) {
        g.setOpacity(0.4f);
        g.strokePath(pathForWindow(window, height), juce::PathStrokeType(3.5f));
      }

      // Hilighted center column
      g.setColour(req.highlight);
      drawCenterColumn(g, height);

      return image;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Collector)
  };

  GrainSynth &synth;
  CoverageFilter coverage;

  std::mutex requestMutex;
  Request request;

  std::mutex imageMutex;
  std::unique_ptr<juce::Image> image;

  std::mutex collectorMutex;
  std::unique_ptr<Collector> collector;
  float maxWidthForNextCollector{0.f};

  Request latestRequest() {
    std::lock_guard<std::mutex> guard(requestMutex);
    return request;
  }

  std::unique_ptr<juce::Image> renderImage() {
    auto req = latestRequest();
    auto width = req.bounds.getWidth();
    if (width < 1) {
      return nullptr;
    }
    auto latestSound = synth.latestSound();
    if (latestSound == nullptr) {
      return nullptr;
    }
    visualizeSoundSettings(*latestSound);
    std::unique_ptr<Collector> cptr;
    {
      std::lock_guard<std::mutex> guard(collectorMutex);
      cptr = std::make_unique<Collector>(width, maxWidthForNextCollector);
      maxWidthForNextCollector = 0.f;
      std::swap(collector, cptr);
    }
    return cptr == nullptr ? nullptr : cptr->renderImage(req, coverage);
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImageRender)
};

WavePanel::WavePanel(RvvProcessor &p)
    : processor(p), image(std::make_unique<ImageRender>(processor.synth)) {
  processor.synth.addListener(image.get());
  image->startThread();
  image->addChangeListener(this);
}

WavePanel::~WavePanel() {
  processor.synth.removeListener(image.get());
  image->signalThreadShouldExit();
  image->notify();
  image->waitForThreadToExit(-1);
}

void WavePanel::resized() {
  image->requestChange(ImageRender::Request{
      .bounds = getLocalBounds(),
      .background = findColour(juce::ResizableWindow::backgroundColourId),
      .highlight = findColour(juce::Slider::thumbColourId),
  });
}

void WavePanel::paint(juce::Graphics &g) {
  auto bounds = getLocalBounds().toFloat();
  image->drawLatest(g, bounds);
}

void WavePanel::changeListenerCallback(juce::ChangeBroadcaster *) {
  const juce::MessageManagerLock mmlock;
  repaint();
}
