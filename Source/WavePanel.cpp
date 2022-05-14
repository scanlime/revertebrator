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
    static constexpr auto maxFrameRate = 30.;
    auto minTicks = juce::Time::secondsToHighResolutionTicks(1. / maxFrameRate);
    auto lastTimestamp = juce::Time::getHighResolutionTicks();
    while (!threadShouldExit()) {
      auto timestamp = juce::Time::getHighResolutionTicks();
      if (juce::int64(timestamp - lastTimestamp) < minTicks) {
        wait(1);
        continue;
      }
      lastTimestamp = timestamp;
      auto nextImage = renderImage();
      if (nextImage != nullptr) {
        {
          std::lock_guard<std::mutex> guard(imageMutex);
          std::swap(image, nextImage);
        }
        sendChangeMessage();
      }
      wait(-1);
    }
  }

private:
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

      juce::Image coverageMaskImage() {
        float peak = 0.f;
        for (auto value : coverage) {
          peak = std::max(peak, value);
        }
        float normalize = peak > 0.f ? 1.f / peak : 0.f;
        const int height = 10;
        const auto format = juce::Image::SingleChannel;
        juce::Image result(format, coverage.size(), height, true);
        juce::Graphics g(result);
        for (int x = 0; x < coverage.size(); x++) {
          juce::uint8 alpha = std::round(coverage[x] * 255.f / peak);
          g.setColour(juce::Colour().withAlpha(alpha));
          g.drawVerticalLine(x, 1, height - 1);
        }
        return std::move(result);
      }
    };

    std::vector<GrainWaveform::Window> windows;
    std::unordered_map<GrainWaveform *, WaveInfo> waves;
    int numColumns, samplesPerTimeStep;
    float samplesPerColumn;

    int centerColumn() const { return numColumns / 2; }

    void drawCenterColumn(juce::Graphics &g, int height) {
      auto thick = height * 0.02f;
      auto margin = 1;
      auto x = centerColumn();
      g.drawLine(x, margin + thick, x, height - thick - margin, thick);
      g.fillEllipse(x - thick, margin, thick * 2, thick * 2);
      g.fillEllipse(x - thick, height - margin - thick * 2, thick * 2,
                    thick * 2);
    }

    void playing(GrainWaveform &wave, const GrainSequence::Point &seq,
                 int sampleNum, int sampleCount) {
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

    std::unique_ptr<juce::Image> renderImage(const Request &req) {
      auto height = req.bounds.getHeight();
      if (height < 1) {
        return nullptr;
      }
      auto image = std::make_unique<juce::Image>(juce::Image::RGB, numColumns,
                                                 height, false);

      juce::Graphics g(*image);
      g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
      g.fillAll(req.background);
      g.setColour(req.background.contrasting(1.f));

      // Faint outline of all active window functions
      for (auto window : collectUniqueWaveformWindows()) {
        g.setOpacity(0.2f);
        g.strokePath(pathForWindow(window, height), juce::PathStrokeType(2.f));
      }

      // Coverage (antialiased playback positions) for active waveforms
      for (auto &item : waves) {
        g.setOpacity(0.5f);
        auto mask = item.second.coverageMaskImage();
        g.drawImage(mask, 0, 0, numColumns, height, 0, 0, numColumns,
                    mask.getHeight(), true);
      }

      // Hilighted window functions
      g.setColour(req.highlight);
      for (auto &window : windows) {
        g.setOpacity(0.4f);
        g.strokePath(pathForWindow(window, height), juce::PathStrokeType(3.5f));
      }

      // Hilighted center column
      g.setColour(req.highlight);
      g.setOpacity(1.f);
      drawCenterColumn(g, height);

      return image;
    }
  };

  GrainSynth &synth;

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
      auto samplesPerColumn = 2.f * maxWidthForNextCollector / width;
      cptr = std::move(std::make_unique<Collector>(Collector{
          .numColumns = width,
          .samplesPerColumn = samplesPerColumn,
      }));
      maxWidthForNextCollector = 0.f;
      std::swap(collector, cptr);
    }
    return cptr == nullptr ? nullptr : cptr->renderImage(req);
  }
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
  image->notify();
}

void WavePanel::changeListenerCallback(juce::ChangeBroadcaster *) {
  const juce::MessageManagerLock mmlock;
  repaint();
}
