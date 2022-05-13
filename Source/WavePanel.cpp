#include "WavePanel.h"
#include <unordered_map>

class WavePanel::ImageRender : public juce::Thread,
                               public juce::ChangeBroadcaster,
                               public GrainVoice::Listener {
public:
  ImageRender() : Thread("wave-image") {}
  ~ImageRender() override {}

  struct Request {
    juce::Rectangle<int> bounds;
    juce::Colour background;
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
    static constexpr int fps = 20;
    while (!threadShouldExit()) {
      wait(1000 / fps);
      auto nextImage = renderImage();
      {
        std::lock_guard<std::mutex> guard(imageMutex);
        std::swap(image, nextImage);
      }
      sendChangeMessage();
    }
  }

  void grainVoicePlaying(const GrainVoice &voice, const GrainSound &sound,
                         GrainWaveform &wave, const GrainSequence::Point &seq,
                         int sampleNum) override {
    auto maxWidth = sound.maxGrainWidthSamples();
    std::lock_guard<std::mutex> guard(collectorMutex);
    maxWidthForNextCollector = std::max(maxWidthForNextCollector, maxWidth);
    if (collector) {
      collector->updateVoice(voice, wave, seq.gain, sampleNum);
    }
  }

private:
  struct Collector {
  public:
    struct Column {
      float y{0.f};
      bool playing{false};
    };

    std::vector<Column> columns;
    float samplesPerColumn;

    Collector(int numColumns, float samplesPerColumn)
        : columns(numColumns), samplesPerColumn(samplesPerColumn) {}

    int middleColumn() { return columns.size() / 2; }

    void updateVoice(const GrainVoice &voice, GrainWaveform &wave, float gain,
                     int sampleNum) {
      printf("collector goes here, wave %p, %d columns, %f spc\n", &wave,
             columns.size(), samplesPerColumn);
    }

    std::unique_ptr<juce::Image> renderImage(const Request &req) {
      printf("render\n");
      return nullptr;
    }
  };

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
    auto nextSamplesPerColumn = 2.f * maxWidthForNextCollector / width;
    auto c = std::make_unique<Collector>(width, nextSamplesPerColumn);
    maxWidthForNextCollector = 0.f;
    {
      std::lock_guard<std::mutex> guard(collectorMutex);
      std::swap(collector, c);
    }
    if (c == nullptr) {
      return nullptr;
    } else {
      return c->renderImage(req);
    }
  }
};

WavePanel::WavePanel(RvvProcessor &p)
    : processor(p), image(std::make_unique<ImageRender>()) {
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
