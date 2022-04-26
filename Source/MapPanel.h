#pragma once

#include "AudioProcessor.h"
#include <JuceHeader.h>

class MapImage : private juce::TimeSliceClient, public juce::ChangeBroadcaster {
public:
  MapImage(GrainData &, juce::TimeSliceThread &);
  ~MapImage() override;

  struct Request {
    juce::Rectangle<int> bounds;
    juce::Colour background;
  };

  void requestChange(const Request &req);
  void drawLatestImage(juce::Graphics &g, juce::Rectangle<float> location);
  void discardStoredImage();

private:
  int useTimeSlice() override;

  GrainData &grainData;
  juce::TimeSliceThread &thread;
  juce::CriticalSection mutex;
  std::unique_ptr<juce::Image> image;
  Request lastRequest, nextRequest;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MapImage)
};

class MapPanel : public juce::Component,
                 private juce::Value::Listener,
                 private juce::ChangeListener {
public:
  MapPanel(AudioProcessor &, juce::TimeSliceThread &);
  ~MapPanel() override;

  void paint(juce::Graphics &) override;
  void resized() override;
  void mouseMove(const juce::MouseEvent &) override;

private:
  AudioProcessor &audioProcessor;
  juce::Value grainDataStatus;
  MapImage mapImage;

  void requestNewImage();
  void valueChanged(juce::Value &) override;
  void changeListenerCallback(juce::ChangeBroadcaster *) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MapPanel)
};
