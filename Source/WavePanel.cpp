#include "WavePanel.h"
#include <unordered_map>

class WaveCollector {
public:
  void updateVoice(const GrainVoice &voice, GrainWaveform &wave, float gain,
                   int sampleNum) {
    std::lock_guard<std::mutex> guard(voicesMutex);
    auto &state = voices[&voice];
    state.wave = wave;
    state.gain = gain;
    state.sampleNum = sampleNum;
  }

  struct WaveState {
    GrainWaveform::Ptr wave;
    float totalGain{0.f};
    std::vector<int> positionForEachVoice;
  };

  using Waves = std::unordered_map<const GrainWaveform *, WaveState>;

  Waves collect() {
    Voices collectedVoices;
    Waves collectedWaves;
    {
      std::lock_guard<std::mutex> guard(voicesMutex);
      collectedVoices.swap(voices);
    }
    for (auto &voice : collectedVoices) {
      auto &state = collectedWaves[voice.second.wave.get()];
      state.wave = voice.second.wave;
      state.totalGain += voice.second.gain;
      state.positionForEachVoice.push_back(voice.second.sampleNum);
    }
    return collectedWaves;
  }

private:
  struct VoiceState {
    GrainWaveform::Ptr wave;
    float gain;
    int sampleNum;
  };

  using Voices = std::unordered_map<const GrainVoice *, VoiceState>;
  std::mutex voicesMutex;
  Voices voices;
};

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
    waves.updateVoice(voice, wave, seq.gain, sampleNum);
  }

private:
  std::mutex requestMutex;
  Request request;
  std::mutex imageMutex;
  std::unique_ptr<juce::Image> image;
  WaveCollector waves;

  Request latestRequest() {
    std::lock_guard<std::mutex> guard(requestMutex);
    return request;
  }

  struct ColumnInfo {};

  std::unique_ptr<juce::Image> renderImage() {
    auto req = latestRequest();
    auto width = req.bounds.getWidth(), height = req.bounds.getHeight();
    auto image = std::make_unique<juce::Image>(
        juce::Image::RGB, std::max(1, width), std::max(1, height), false);

    auto collectedWaves = waves.collect();
    std::vector<ColumnInfo> columns(width);
    for (auto &wave : collectedWaves) {
      printf("Wave %p, %d positions, gain %f, window %f %d %d %d\n",
             wave.second.wave.get(), wave.second.positionForEachVoice.size(),
             wave.second.totalGain, wave.second.wave->key.window.mix,
             wave.second.wave->key.window.width0,
             wave.second.wave->key.window.width1,
             wave.second.wave->key.window.phase1);
    }

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
