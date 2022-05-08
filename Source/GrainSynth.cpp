#include "GrainSynth.h"

GrainSequence::GrainSequence(GrainIndex &index, const Params &p, const Midi &m)
    : index(index), params(p), midi(m) {}

GrainSequence::~GrainSequence() {}

GrainSequence::Point GrainSequence::generate() {
  auto selNoise = params.selSpread * (prng() / double(prng.max()));
  auto pitchNoise = params.pitchSpread * (prng() / double(prng.max()));

  auto pitchBend = midi.pitchWheel / 8192.0 - 1.0;
  auto modWheel = midi.modWheel / 64.0 - 1.0;
  auto gainDb = juce::jmap(midi.velocity, params.gainDbLow, params.gainDbHigh);

  auto sel = params.selCenter + modWheel * params.selMod + selNoise;
  auto sel01 = juce::jlimit<float>(0.f, 1.f, std::fmod(sel + 2., 1.));

  auto semitones = midi.note + params.pitchBendRange * pitchBend + pitchNoise;
  auto hz = 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0);

  auto bin = index->closestBinForPitch(hz / params.speedWarp);
  auto grains = index->grainsForBin(bin);
  unsigned g = std::round(grains.getStart() + sel01 * (grains.getLength() - 1));
  return {grain : g, gain : juce::Decibels::decibelsToGain(gainDb)};
}

GrainSynth::GrainSynth(GrainData &grainData, int numVoices) {
  for (auto i = 0; i < juce::numElementsInArray(lastModWheelValues); i++) {
    lastModWheelValues[i] = 64;
  }
  for (auto i = 0; i < numVoices; i++) {
    addVoice(new GrainVoice(grainData));
  }
}

GrainSynth::~GrainSynth() {}

void GrainSynth::changeSound(GrainIndex &index,
                             const GrainSound::Params &params) {
  auto newSound = new GrainSound(index, params);
  juce::ScopedLock sl(lock);
  sounds.clear();
  sounds.add(newSound);
}

void GrainSynth::noteOn(int channel, int midiNote, float velocity) {
  juce::ScopedLock sl(lock);
  for (auto sound : sounds) {
    if (sound->appliesToNote(midiNote) && sound->appliesToChannel(channel)) {
      auto voice =
          findFreeVoice(sound, channel, midiNote, isNoteStealingEnabled());
      voice->controllerMoved(0x01, lastModWheelValues[channel]);
      startVoice(voice, sound, channel, midiNote, velocity);
    }
  }
}

void GrainSynth::handleController(int channel, int controller, int value) {
  if (controller == 0x01) {
    lastModWheelValues[channel] = value;
  }
  juce::Synthesiser::handleController(channel, controller, value);
}

GrainSound::GrainSound(GrainIndex &index, const Params &params)
    : index(index), params(params),
      speedRatio(index.sampleRate / params.sampleRate *
                 params.sequence.speedWarp),
      window(index.maxGrainWidthSamples() / speedRatio, params.window) {}

GrainSound::~GrainSound() {}
bool GrainSound::appliesToNote(int) { return true; }
bool GrainSound::appliesToChannel(int) { return true; }
GrainIndex &GrainSound::getIndex() { return *index; }

double GrainSound::grainRepeatsPerSample() const {
  return params.grainRate / params.sampleRate;
}

int GrainSound::windowSizeInSamples() const {
  return window.range().getLength();
}

GrainWaveform::Key GrainSound::waveformKeyForGrain(unsigned grain) const {
  return {
      .grain = grain,
      .speedRatio = speedRatio,
      .window = window,
  };
}

GrainSequence::Ptr GrainSound::grainSequence(const GrainSequence::Midi &midi) {
  return std::make_unique<GrainSequence>(getIndex(), params.sequence, midi);
}

GrainVoice::GrainVoice(GrainData &grainData) : grainData(grainData) {}
GrainVoice::~GrainVoice() {}

bool GrainVoice::canPlaySound(juce::SynthesiserSound *sound) {
  return dynamic_cast<GrainSound *>(sound) != nullptr;
}

void GrainVoice::startNote(int midiNote, float velocity,
                           juce::SynthesiserSound *genericSound,
                           int currentPitchWheelPosition) {
  auto sound = dynamic_cast<GrainSound *>(genericSound);
  if (sound != nullptr) {
    sequence = sound->grainSequence({
        .note = midiNote,
        .pitchWheel = currentPitchWheelPosition,
        .modWheel = currentModWheelPosition,
        .velocity = velocity,
    });
    queue.clear();
    sampleOffsetInQueue = 0;
  }
}

void GrainVoice::stopNote(float, bool) { sequence = nullptr; }

void GrainVoice::pitchWheelMoved(int newValue) {
  if (sequence != nullptr) {
    sequence->midi.pitchWheel = newValue;
  }
}

void GrainVoice::controllerMoved(int controllerNumber, int newValue) {
  if (controllerNumber == 0x01) {
    currentModWheelPosition = newValue;
    if (sequence != nullptr) {
      sequence->midi.modWheel = newValue;
    }
  }
}

void GrainVoice::renderNextBlock(juce::AudioBuffer<float> &outputBuffer,
                                 int startSample, int numSamples) {
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound == nullptr) {
    return;
  }
  while (numSamples > 0) {
    fillQueueToDepth(1 + 2 * sound->windowSizeInSamples() *
                             sound->grainRepeatsPerSample());
    fetchQueueWaveforms();
    if (queue.empty()) {
      // No more work
      clearCurrentNote();
      return;
    }
    int progress =
        renderFromQueue(*sound, outputBuffer, startSample, numSamples);
    if (progress <= 0) {
      // Stalled momentarily waiting for data to load
      return;
    }
    startSample += progress;
    numSamples -= progress;
  }
}

int GrainVoice::renderFromQueue(GrainSound &sound,
                                juce::AudioBuffer<float> &outputBuffer,
                                int startSample, int numSamples) {
  int progress = 0;
  auto repeatRate = sound.grainRepeatsPerSample();

  // if (repeatRate > 0.) {
  // tbd
  // } else {

  for (auto &grain : queue) {
    if (grain.wave == nullptr) {
      break;
    }
    auto &wave = *grain.wave;
    auto size =
        std::min(numSamples, wave.buffer.getNumSamples() - sampleOffsetInQueue);
    for (int ch = 0; ch < outputBuffer.getNumChannels(); ch++) {
      outputBuffer.addFrom(ch, startSample, wave.buffer,
                           ch % wave.buffer.getNumChannels(),
                           sampleOffsetInQueue, size, grain.seq.gain);
    }
    progress += size;
    break;
  }

  sampleOffsetInQueue += progress;
  while (!queue.empty() && queue.front().wave &&
         sampleOffsetInQueue > queue.front().wave->buffer.getNumSamples()) {
    sampleOffsetInQueue -= queue.front().wave->buffer.getNumSamples();
    queue.pop_front();
  }

  printf("rq %d-%d #%d +%d -> %d\n", startSample, numSamples, queue.size(),
         sampleOffsetInQueue, progress);
  return progress;
}

void GrainVoice::fillQueueToDepth(int numGrains) {
  if (sequence != nullptr) {
    while (queue.size() < numGrains) {
      queue.push_back({sequence->generate()});
    }
  }
}

void GrainVoice::fetchQueueWaveforms() {
  auto soundPtr = getCurrentlyPlayingSound();
  auto sound = dynamic_cast<GrainSound *>(soundPtr.get());
  if (sound) {
    for (auto &grain : queue) {
      if (grain.wave == nullptr) {
        grain.wave = grainData.getWaveform(
            sound->getIndex(), sound->waveformKeyForGrain(grain.seq.grain));
      }
    }
  }
}
