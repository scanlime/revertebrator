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
    static constexpr float minTimeStep = 1.f / 30.f, maxTimeStep = 1.f / 10.f;
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
  struct Collector {
  public:
    struct WaveInfo {
      GrainWaveform::Ptr wave;
      juce::Image coverage;
    };

    std::vector<GrainWaveform::Window> windows;
    std::unordered_map<GrainWaveform *, WaveInfo> waves;
    int numColumns, samplesPerTimeStep;
    float samplesPerColumn;

    int centerColumn() const { return numColumns / 2; }

    void waveInfo(GrainWaveform &wave) {
      auto &slot = waves[&wave];
      if (slot.wave == nullptr) {
        slot.wave = &wave;
        slot.coverage =
            juce::Image(juce::Image::SingleChannel, numColumns, 1, true);
      }
      jassert(slot.wave == &wave);
      jassert(slot.coverage.getWidth() == numColumns);
    }

    void playing(GrainWaveform &wave, const GrainSequence::Point &seq,
                 int sampleNum) {
      // auto x0 = std::max<int>(
      //     0, centerColumn() + (sampleNum +
      //     wave.key.window.range().getStart()) /
      //                             samplesPerColumn);
      // auto x1 = std::min<int>(columns.size() - 1,
      //                         1. + centerColumn() +
      //                             (sampleNum + samplesPerTimeStep +
      //                              wave.key.window.range().getStart()) /
      //                                 samplesPerColumn);
      // for (auto x = x0; x < x1; x++) {
      //   columns[x].playbackGain += seq.gain;
      // }
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
      //
      // juce::Path path;
      // auto m = 3;
      // for (int x = 0; x < numColumns; x++) {
      //   auto y = m + (height - 1 - m - m) * (1.f - columns[x].envelope);
      //   if (x == 0) {
      //     path.startNewSubPath(x, y);
      //   } else {
      //     path.lineTo(x, y);
      //   }
      // }
      // g.setColour(req.background.contrasting(1.f));
      // g.setOpacity(0.7f);
      // g.strokePath(path, juce::PathStrokeType(0.5f * m));
      // g.setOpacity(0.2f);
      // for (int x = 0; x < numColumns; x++) {
      //   if (columns[x].playbackGain > 0.f) {
      //     g.drawVerticalLine(x, 0, height);
      //   }
      // }

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
