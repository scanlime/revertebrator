#include "GrainSynth.h"

GrainSequence::Point GrainSequence::generate(std::mt19937 &prng) {
  std::uniform_real_distribution<> uniform(-1., 1.);
  auto selNoise = params.selSpread * uniform(prng);
  auto pitchNoise = params.pitchSpread * uniform(prng);

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
  std::mt19937 prng;
  for (auto i = 0; i < juce::numElementsInArray(lastModWheelValues); i++) {
    lastModWheelValues[i] = 64;
  }
  for (auto i = 0; i < numVoices; i++) {
    unsigned seed = prng();
    addVoice(new GrainVoice(grainData, std::mt19937(seed)));
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

double GrainSound::grainRepeatsPerSecond() const { return params.grainRate; }

double GrainSound::grainRepeatsPerSample() const {
  return params.grainRate / params.sampleRate;
}

GrainWaveform::Key GrainSound::waveformKeyForGrain(unsigned grain) const {
  return {
      .grain = grain,
      .speedRatio = speedRatio,
      .window = window,
  };
}

GrainSequence::Ptr GrainSound::grainSequence(const GrainSequence::Midi &midi) {
  return std::make_unique<GrainSequence>(
      GrainSequence{getIndex(), params.sequence, midi});
}

GrainVoice::GrainVoice(GrainData &grainData, const std::mt19937 &prng)
    : grainData(grainData), prng(prng) {}
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
    sampleOffsetInQueue = 0;
    queue.clear();
    fillQueueToPreloadSound(*sound);
    fetchQueueWaveforms(*sound);
  }
}

void GrainVoice::stopNote(float, bool) {
  sequence = nullptr;
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound == nullptr) {
    queue.clear();
  } else {
    queue.resize(numActiveGrainsInQueue(*sound));
  }
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
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound == nullptr) {
    return;
  }
  fillQueueToPreloadSound(*sound);
  fetchQueueWaveforms(*sound);
  if (queue.empty()) {
    // No more work
    clearCurrentNote();
  } else {
    renderFromQueue(*sound, outputBuffer, startSample, numSamples);
  }
}

void GrainVoice::fillQueueToPreloadSound(const GrainSound &sound) {
  auto preloadSeconds = 1.;
  auto preloadGrains = 1. + sound.grainRepeatsPerSecond() * preloadSeconds;
  fillQueueToDepth(std::ceil(preloadGrains));
}

void GrainVoice::fillQueueToDepth(int numGrains) {
  if (sequence != nullptr) {
    while (queue.size() < numGrains) {
      queue.push_back({sequence->generate(prng)});
    }
  }
}

void GrainVoice::fetchQueueWaveforms(GrainSound &sound) {
  for (auto &grain : queue) {
    if (grain.wave == nullptr) {
      grain.wave = grainData.getWaveform(
          sound.getIndex(), sound.waveformKeyForGrain(grain.seq.grain));
    }
  }
}

static inline bool timestampForNextRepeat(int &timestamp, double rate) {
  constexpr int maxIntervalToConsider = 1 << 22;
  const double minRateToConsider = 1. / maxIntervalToConsider;
  if (rate > minRateToConsider) {
    timestamp += int(std::ceil(1. / rate));
    return true;
  } else {
    return false;
  }
}

int GrainVoice::numActiveGrainsInQueue(const GrainSound &sound) {
  auto repeatRate = sound.grainRepeatsPerSample();
  int queueTimestamp = 0;
  int numActive = 0;

  for (auto &grain : queue) {
    if (grain.wave == nullptr) {
      // Stalled, can't be active yet
      break;
    }
    if (queueTimestamp > sampleOffsetInQueue) {
      // Hasn't happened yet
      break;
    }
    numActive++;
    if (!timestampForNextRepeat(queueTimestamp, repeatRate)) {
      // Not repeating
      break;
    }
  }
  return numActive;
}

void GrainVoice::renderFromQueue(const GrainSound &sound,
                                 juce::AudioBuffer<float> &outputBuffer,
                                 int startSample, int numSamples) {
  auto repeatRate = sound.grainRepeatsPerSample();
  {
    int queueTimestamp = 0;
    for (auto &grain : queue) {
      if (grain.wave == nullptr) {
        // Stalled while we load grains, leave without advancing the queue
        return;
      }
      if (queueTimestamp > (sampleOffsetInQueue + numSamples)) {
        // Happens after the end of this render block
        break;
      }

      auto &wave = *grain.wave;
      auto gain = grain.seq.gain;
      auto srcSize = wave.buffer.getNumSamples();
      auto inChannels = wave.buffer.getNumChannels();
      auto outChannels = outputBuffer.getNumChannels();

      // Figure out where this grain goes relative to the block we are rendering
      auto relative = queueTimestamp - sampleOffsetInQueue;
      auto copySource = std::max<int>(0, -relative);
      auto copyDest = std::max<int>(0, relative);
      auto copySize = std::min(numSamples - copyDest, srcSize - copySource);

      if (copySize > 0) {
        for (int ch = 0; ch < outChannels; ch++) {
          outputBuffer.addFrom(ch, startSample + copyDest, wave.buffer,
                               ch % inChannels, copySource, copySize, gain);
        }
      }
      if (!timestampForNextRepeat(queueTimestamp, repeatRate)) {
        break;
      }
    }
  }
  // Advance past the rendered block, and remove grains we're fully done with
  sampleOffsetInQueue += numSamples;
  while (!queue.empty()) {
    auto &wave = queue.front().wave;
    if (wave == nullptr) {
      break;
    }
    if (sampleOffsetInQueue < wave->buffer.getNumSamples()) {
      // Still using this grain
      break;
    }
    int queueTimestamp = 0;
    if (timestampForNextRepeat(queueTimestamp, repeatRate)) {
      sampleOffsetInQueue -= queueTimestamp;
      queue.pop_front();
    } else {
      queue.clear();
    }
  }
}
