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

GrainSound::GrainSound(GrainIndex &index, const Params &params)
    : index(index), params(params),
      speedRatio(index.sampleRate / params.sampleRate *
                 params.sequence.speedWarp),
      window(index.maxGrainWidthSamples() / speedRatio, params.window) {}

GrainSound::~GrainSound() {}
bool GrainSound::appliesToNote(int) { return true; }
bool GrainSound::appliesToChannel(int) { return true; }
GrainIndex &GrainSound::getIndex() { return *index; }

GrainWaveform::Key GrainSound::waveformForGrain(unsigned grain) {
  return {
      .grain = grain,
      .speedRatio = speedRatio,
      .window = window,
  };
}

std::unique_ptr<GrainSequence>
GrainSound::grainSequence(const GrainSequence::Midi &midi) {
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
  GrainSequence::Midi midi = {
      .note = midiNote,
      .pitchWheel = currentPitchWheelPosition,
      .modWheel = currentModWheelPosition,
      .velocity = velocity,
  };
  auto sound = dynamic_cast<GrainSound *>(genericSound);

  if (sound != nullptr) {
    sequence = sound->grainSequence(midi);
    queue.clear();
    temp_sample = 0;
    temp_wave = nullptr;
  }
}

void GrainVoice::stopNote(float, bool) {
  sequence = nullptr;
  queue.clear();
}

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
  auto genericSound = getCurrentlyPlayingSound();
  auto sound = dynamic_cast<GrainSound *>(genericSound.get());
  if (sequence == nullptr || sound == nullptr) {
    return;
  }

  while (queue.size() < 4) {
    queue.push_back(sequence->generate());
  }
  for (auto point : queue) {
    grainData.getWaveform(sound->getIndex(),
                          sound->waveformForGrain(point.grain));
  }
  if (temp_wave == nullptr) {
    GrainWaveform::Ptr next = grainData.getWaveform(
        sound->getIndex(), sound->waveformForGrain(queue.front().grain));
    if (next) {
      temp_wave = next;
      temp_sample = 0;
      temp_gain = queue.front().gain;
    }
    queue.pop_front();
  }
  if (temp_wave != nullptr) {
    auto size =
        std::min(numSamples, temp_wave->buffer.getNumSamples() - temp_sample);

    for (int ch = 0; ch < outputBuffer.getNumChannels(); ch++) {
      outputBuffer.addFrom(ch, startSample, temp_wave->buffer,
                           ch % temp_wave->buffer.getNumChannels(), temp_sample,
                           size, temp_gain);
    }
    temp_sample += size;
    if (temp_sample >= temp_wave->buffer.getNumSamples()) {
      temp_wave = nullptr;
    }
  }
}

GrainSynth::GrainSynth(GrainData &grainData, int numVoices) {
  for (auto i = 0; i < 16; i++) {
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
