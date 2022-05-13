#include "WavePanel.h"

class WavePanel::ImageRender : public juce::Thread,
                               public juce::ChangeBroadcaster,
                               public GrainVoice::Listener {
public:
  ImageRender() : Thread("wave-image") {}
  ~ImageRender() override {}

  struct Request {
    juce::Rectangle<int> bounds;
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

  void grainVoicePlaying(const GrainVoice &voice, const GrainSound &sound,
                         GrainWaveform &wave, const GrainSequence::Point &seq,
                         int sampleNum) override {
    // std::lock_guard<std::mutex> guard(wavesMutex);
    // auto &waveInfo = waves.getReference(WaveInfo::Key(&wave));
    // waveInfo.ptr = wave;
  }

  void run() override {
    static constexpr int fps = 20;
    while (!threadShouldExit()) {
      wait(1000 / fps);
      auto nextImage = renderImage();
      {
        std::lock_guard<std::mutex> guard(imageMutex);
        image = std::move(nextImage);
      }
      sendChangeMessage();
    }
  }

private:
  std::mutex requestMutex;
  Request request;

  std::mutex imageMutex;
  std::unique_ptr<juce::Image> image;

  Request latestRequest() {
    std::lock_guard<std::mutex> guard(requestMutex);
    return request;
  }

  std::unique_ptr<juce::Image> renderImage() {
    auto req = latestRequest();
    auto width = req.bounds.getWidth(), height = req.bounds.getHeight();
    auto image = std::make_unique<juce::Image>(
        juce::Image::RGB, std::max(1, width), std::max(1, height), false);

    juce::Image::BitmapData bits(*image, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        static int b = 0;
        b++;
        auto c = juce::Colour(0xff000000 | b);
        bits.setPixelColour(x, y, c);
      }
    }

    return image;
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
