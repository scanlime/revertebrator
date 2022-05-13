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

  void grainVoicePlaying(const GrainVoice &voice, const GrainSound &sound,
                         GrainWaveform &wave, const GrainSequence::Point &seq,
                         int sampleNum) override {
    auto maxWidth = sound.maxGrainWidthSamples();
    std::lock_guard<std::mutex> guard(collectorMutex);
    maxWidthForNextCollector = std::max(maxWidthForNextCollector, maxWidth);
    if (collector) {
      collector->playing(voice, wave, seq.gain, sampleNum);
    }
  }

  void visualizeSoundSettings(const GrainSound &sound) {
    auto maxWidth = sound.maxGrainWidthSamples();
    std::lock_guard<std::mutex> guard(collectorMutex);
    maxWidthForNextCollector = std::max(maxWidthForNextCollector, maxWidth);
    if (collector) {
      collector->visualizeSoundSettings(sound);
    }
  }

private:
  struct Collector {
  public:
    Collector(int numColumns, float samplesPerColumn, int samplesPerTimeStep)
        : columns(numColumns), samplesPerColumn(samplesPerColumn),
          samplesPerTimeStep(samplesPerTimeStep) {
      jassert(numColumns >= 1);
      jassert(samplesPerColumn > 0.f);
    }

    int centerColumn() { return columns.size() / 2; }

    void playing(const GrainVoice &voice, GrainWaveform &wave, float gain,
                 int sampleNum) {
      auto x0 = std::max<int>(
          0, centerColumn() + (sampleNum + wave.key.window.range().getStart()) /
                                  samplesPerColumn);
      auto x1 = std::min<int>(columns.size() - 1,
                              1. + centerColumn() +
                                  (sampleNum + samplesPerTimeStep +
                                   wave.key.window.range().getStart()) /
                                      samplesPerColumn);
      for (auto x = x0; x < x1; x++) {
        columns[x].playbackGain += gain;
      }
    }

    void visualizeSoundSettings(const GrainSound &sound) {
      for (int x = 0; x < columns.size(); x++) {
        columns[x].envelope =
            sound.getWindow().evaluate((x - centerColumn()) * samplesPerColumn);
      }
    }

    std::unique_ptr<juce::Image> renderImage(const Request &req) {
      auto height = req.bounds.getHeight();
      if (height < 1) {
        return nullptr;
      }
      auto image = std::make_unique<juce::Image>(juce::Image::RGB,
                                                 columns.size(), height, false);
      juce::Graphics g(*image);
      g.fillAll(req.background);

      juce::Path path;
      auto m = 3;
      for (int x = 0; x < columns.size(); x++) {
        auto y = m + (height - 1 - m - m) * (1.f - columns[x].envelope);
        if (x == 0) {
          path.startNewSubPath(x, y);
        } else {
          path.lineTo(x, y);
        }
      }
      g.setColour(req.background.contrasting(1.f));
      g.setOpacity(0.7f);
      g.strokePath(path, juce::PathStrokeType(0.5f * m));
      g.setOpacity(0.2f);
      for (int x = 0; x < columns.size(); x++) {
        if (columns[x].playbackGain > 0.f) {
          g.drawVerticalLine(x, 0, height);
        }
      }

      g.setColour(req.highlight);
      g.drawVerticalLine(centerColumn(), 0, height);

      return image;
    }

    struct Column {
      float playbackGain{0.f}, envelope{0.f};
      juce::Range<float> audioRange;
    };

    std::vector<Column> columns;
    float samplesPerColumn;
    int samplesPerTimeStep;
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
    auto samplesPerTimeStep = latestSound->outputSampleRate() * timeStep;
    std::unique_ptr<Collector> cptr;
    {
      std::lock_guard<std::mutex> guard(collectorMutex);
      auto samplesPerColumn = 2.f * maxWidthForNextCollector / width;
      cptr = std::make_unique<Collector>(width, samplesPerColumn,
                                         samplesPerTimeStep);
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
