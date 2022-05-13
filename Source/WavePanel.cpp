#include "WavePanel.h"
#include <unordered_map>

class WaveCollector {
public:
  WaveCollector(int numColumns)

  class WaveColumns {
  public:
    WaveColumns(int numColumns, const WaveCollector::Results &collected)
        : columns(numColumns),
          samplesPerColumn(2 * collected.maxWidth / numColumns) {
      for (auto &item : collected.waves) {
        addWave(item.second);
      }
    }

    void drawToImage(juce::Image &image, int height, juce::Colour background) {
      auto foreground = background.contrasting(1);
      juce::Image::BitmapData bits(image, juce::Image::BitmapData::writeOnly);
      for (int y = 0; y < height; y++) {
        for (int x = 0; x < columns.size(); x++) {
          auto &column = columns[x];
          auto c = column.playing || y == int((1. - column.y) * (height - 1));
          bits.setPixelColour(x, y, c ? foreground : background);
        }
      }
    }

  private:
    struct ColumnInfo {
      float y{0.f};
      bool playing{false};
    };

    std::vector<ColumnInfo> columns;
    float samplesPerColumn;

    void addWave(const WaveCollector::WaveState &state) {
      auto window = state.wave->key.window;
      for (auto &pos : state.positionForEachVoice) {
        int col = (pos + window.range().getStart()) / samplesPerColumn +
                  columns.size() / 2;
        if (col >= 0 && col < columns.size()) {
          columns[col].playing = true;
        }
      }
      for (int i = 0; i < columns.size(); i++) {
        auto x = (i - (columns.size() / 2)) * samplesPerColumn;
        // columns[i].y += window.evaluate(x);
        if (x > window.range().getStart() && x < window.range().getEnd()) {
          auto ix = x - window.range().getStart();
          if (ix < state.wave->buffer.getNumSamples()) {
            columns[i].y += state.wave->buffer.getSample(0, ix);
          }
        }
      }
    }
  };

  void updateVoice(const GrainVoice &voice, GrainWaveform &wave,
                   float maxGrainWidthSamples, float gain, int sampleNum) {
    std::lock_guard<std::mutex> guard(voicesMutex);
    maxWidth = std::max(maxWidth, maxGrainWidthSamples);
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

  struct Results {
    using Waves = std::unordered_map<const GrainWaveform *, WaveState>;
    Waves waves;
    float maxWidth{0.f};
  };

  Results collect() {
    Voices collectedVoices;
    Results results;
    {
      std::lock_guard<std::mutex> guard(voicesMutex);
      collectedVoices.swap(voices);
      results.maxWidth = maxWidth;
      maxWidth = 0.f;
    }
    for (auto &voice : collectedVoices) {
      auto &state = results.waves[voice.second.wave.get()];
      state.wave = voice.second.wave;
      state.totalGain += voice.second.gain;
      state.positionForEachVoice.push_back(voice.second.sampleNum);
    }
    return results;
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
  float maxWidth{0.f};
};

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
    waves.updateVoice(voice, wave, sound.maxGrainWidthSamples(), seq.gain,
                      sampleNum);
  }

private:
  std::mutex requestMutex;
  Request request;
  std::mutex imageMutex;
  std::unique_ptr<juce::Image> image;

// YOU ARE HERE. Moving the 'request' and 'columns' stuff to WaveCollector,
//   planning on having it wrap up the number of columns and the width scaling
//   and it can go right from the playing callback to an update in the columns array.
xxx

  std::unique_ptr<WaveCollector> collector;
  WaveCollector waves;

  Request latestRequest() {
    std::lock_guard<std::mutex> guard(requestMutex);
    return request;
  }

  std::unique_ptr<juce::Image> renderImage() {
    auto req = latestRequest();
    auto width = req.bounds.getWidth(), height = req.bounds.getHeight();
    auto image = std::make_unique<juce::Image>(
        juce::Image::RGB, std::max(1, width), std::max(1, height), false);
    WaveColumns columns(width, waves.collect());
    columns.drawToImage(*image, height, req.background);
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
