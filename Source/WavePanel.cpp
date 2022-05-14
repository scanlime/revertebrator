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
                         int sampleNum) override {
    auto maxWidth = sound.maxGrainWidthSamples();
    std::lock_guard<std::mutex> guard(collectorMutex);
    maxWidthForNextCollector = std::max(maxWidthForNextCollector, maxWidth);
    if (collector) {
      collector->playing(wave, seq, sampleNum);
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
    static constexpr float minTimeStep = 1.f / 90.f, maxTimeStep = 1.f / 10.f;
    auto lastTimestamp = juce::Time::getHighResolutionTicks();
    while (!threadShouldExit()) {
      auto timestamp = juce::Time::getHighResolutionTicks();
      auto timeStep = std::min<float>(
          maxTimeStep,
          juce::Time::highResolutionTicksToSeconds(timestamp - lastTimestamp));
      if (timeStep < minTimeStep) {
        wait(int(std::ceil(2e-4 * (minTimeStep - timeStep))));
        continue;
      }
      lastTimestamp = timestamp;
      auto nextImage = renderImage(timeStep);
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

      void normalizeCoverage() {
        float peak = 0.f;
        for (auto value : coverage) {
          peak = std::max(peak, value);
        }
        if (peak > 0.f) {
          for (auto &value : coverage) {
            value /= peak;
          }
        }
      }
    };

    std::vector<GrainWaveform::Window> windows;
    std::unordered_map<GrainWaveform *, WaveInfo> waves;
    int numColumns, samplesPerTimeStep;
    float samplesPerColumn;

    int centerColumn() const { return numColumns / 2; }

    void playing(GrainWaveform &wave, const GrainSequence::Point &seq,
                 int sampleNum) {
      // Track the playing audio per-waveform
      auto &waveInfo = waves[&wave];
      if (waveInfo.wave == nullptr) {
        waveInfo.wave = &wave;
        waveInfo.coverage.resize(numColumns);
      }
      jassert(waveInfo.wave == &wave);
      jassert(waveInfo.coverage.size() == numColumns);

      // Figure out which part of the waveform is covered by this time step
      auto sampleStart = wave.key.window.range().getStart() + sampleNum;
      auto sampleEnd = sampleStart + samplesPerTimeStep;

      // Convert from waveform samples to graph columns
      waveInfo.addCoverage(
          std::max<float>(0, centerColumn() + sampleStart / samplesPerColumn),
          std::min<float>(numColumns,
                          centerColumn() + sampleEnd / samplesPerColumn),
          seq.gain);
    }

    juce::Path pathForWindow(const GrainWaveform::Window &window, float top,
                             float bottom) const {
      juce::Path path;
      auto peak = window.peakValue();
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

    std::unique_ptr<juce::Image> renderImage(const Request &req) {
      auto height = req.bounds.getHeight();
      if (height < 1) {
        return nullptr;
      }
      auto image = std::make_unique<juce::Image>(juce::Image::RGB, numColumns,
                                                 height, false);
      juce::Graphics g(*image);
      g.fillAll(req.background);

      g.setColour(req.background.contrasting(1.f));
      for (auto &item : waves) {
        g.setOpacity(0.4f);
        g.strokePath(pathForWindow(item.second.wave->key.window, 0.1 * height, 0.9 * height),
                     juce::PathStrokeType(3.5f));

        item.second.normalizeCoverage();
        auto &columns = item.second.coverage;
        for (int col = 0; col < numColumns; col++) {
          if (columns[col] > 0.f) {
            g.setOpacity(std::min<float>(1.f, 0.5f * columns[col]));
            g.drawVerticalLine(col, 0, height);
          }
        }
      }

      g.setColour(req.highlight);
      for (auto &window : windows) {
        g.setOpacity(0.4f);
        g.strokePath(pathForWindow(window, 0.1 * height, 0.9 * height),
                     juce::PathStrokeType(3.5f));
      }

      g.setColour(req.highlight);
      g.drawVerticalLine(centerColumn(), 0, height);

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

  std::unique_ptr<juce::Image> renderImage(float timeStep) {
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
    auto samplesPerTimeStep = int(latestSound->outputSampleRate() * timeStep);
    std::unique_ptr<Collector> cptr;
    {
      std::lock_guard<std::mutex> guard(collectorMutex);
      auto samplesPerColumn = 2.f * maxWidthForNextCollector / width;
      cptr = std::move(std::make_unique<Collector>(Collector{
          .numColumns = width,
          .samplesPerTimeStep = samplesPerTimeStep,
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
