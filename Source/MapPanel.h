#pragma once

#include "GrainData.h"
#include "RvvProcessor.h"
#include <JuceHeader.h>

class MapPanel : public juce::Component,
                 private juce::Value::Listener,
                 private juce::ChangeListener,
                 private juce::Timer {
public:
  MapPanel(RvvProcessor &);
  ~MapPanel() override;

  void paint(juce::Graphics &) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent &) override;
  void mouseUp(const juce::MouseEvent &) override;
  void mouseEnter(const juce::MouseEvent &) override;
  void mouseExit(const juce::MouseEvent &) override;
  void mouseMove(const juce::MouseEvent &) override;
  void mouseDrag(const juce::MouseEvent &) override;

private:
  class Layout;
  class ImageRender;
  class LiveOverlay;

  RvvProcessor &processor;
  std::unique_ptr<ImageRender> image;
  std::unique_ptr<LiveOverlay> live;
  juce::Value grainDataStatus;

  void updateGrainUnderMouse(const juce::MouseEvent &, bool);
  void requestNewImage(GrainIndex &index);

  void timerCallback() override;
  void valueChanged(juce::Value &) override;
  void changeListenerCallback(juce::ChangeBroadcaster *) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MapPanel)
};
