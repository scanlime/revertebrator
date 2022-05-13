#include "GrainSynth.h"

GrainSequence::GrainSequence(GrainIndex &index) : index(index) {}

GrainSequence::~GrainSequence() {}

StationaryGrainSequence::StationaryGrainSequence(GrainIndex &index,
                                                 const Point &value)
    : GrainSequence(index), value(value) {}

StationaryGrainSequence::~StationaryGrainSequence() {}

GrainSequence::Point StationaryGrainSequence::generate(std::mt19937 &) {
  return value;
}

MidiGrainSequence::MidiGrainSequence(GrainIndex &index, const Params &params,
                                     const Midi &midi)
    : GrainSequence(index), params(params), midi(midi) {}

MidiGrainSequence::~MidiGrainSequence() {}

GrainSequence::Point MidiGrainSequence::generate(std::mt19937 &prng) {
  std::uniform_real_distribution<> uniform(-1., 1.);
  auto selNoise = params.selSpread * uniform(prng);
  auto pitchNoise = params.pitchSpread * uniform(prng);

  auto pitchBend = midi.pitchWheel / 8192.0 - 1.0;
  auto modWheel = midi.modWheel / 128.0 - 0.5;
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
  std::mt19937 seedGenerator;
  for (auto i = 0; i < juce::numElementsInArray(lastModWheelValues); i++) {
    lastModWheelValues[i] = 64;
  }
  for (auto i = 0; i < numVoices; i++) {
    addVoice(new GrainVoice(grainData, std::mt19937(seedGenerator())));
  }
}

GrainSynth::~GrainSynth() {}

void GrainSynth::mouseInputForGrain(unsigned grainId, bool isDown,
                                    int sourceId) {
  juce::ScopedLock sl(lock);
  auto sound = dynamic_cast<GrainSound *>(getSound(0).get());
  if (sound == nullptr) {
    return;
  }
  if (isDown && !voiceForInputSource.contains(sourceId)) {
    voiceForInputSource.set(sourceId, dynamic_cast<GrainVoice *>(findFreeVoice(
                                          getSound(0).get(), -1, -1, true)));
  }
  auto voice = voiceForInputSource[sourceId];
  if (voice) {
    if (isDown && grainId < sound->getIndex().numGrains()) {
      constexpr auto velocity = 0.7f;
      if (!voice->isVoiceActive()) {
        // We don't seem to have a direct way to start a juce::SynthesiserVoice
        // without also triggering a midi note that we need to dequeue.
        startVoice(voice, sound, 0, 0, 0);
        voice->clearGrainQueue();
      }
      voice->startGrain(grainId, velocity);
    } else {
      stopVoice(voice, 0, true);
      voiceForInputSource.remove(sourceId);
    }
  }
}

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

void GrainSynth::addListener(GrainVoice::Listener *listener) {
  for (auto *generic : voices) {
    auto voice = dynamic_cast<GrainVoice *>(generic);
    if (voice) {
      voice->addListener(listener);
    }
  }
}

void GrainSynth::removeListener(GrainVoice::Listener *listener) {
  for (auto *generic : voices) {
    auto voice = dynamic_cast<GrainVoice *>(generic);
    if (voice) {
      voice->removeListener(listener);
    }
  }
}

GrainSound::GrainSound(GrainIndex &index, const Params &params)
    : index(index), params(params),
      speedRatio(index.sampleRate / params.sampleRate *
                 params.sequence.speedWarp),
      window(maxGrainWidthSamples(), params.window) {}

GrainSound::~GrainSound() {}
bool GrainSound::appliesToNote(int) { return true; }
bool GrainSound::appliesToChannel(int) { return true; }
GrainIndex &GrainSound::getIndex() { return *index; }

float GrainSound::maxGrainWidthSamples() const {
  return index->maxGrainWidthSamples() / speedRatio;
}

double GrainSound::grainRepeatsPerSample() const {
  return params.grainRate / params.sampleRate;
}

int GrainSound::targetQueueDepth() const {
  return std::ceil(1. + window.range().getLength() * grainRepeatsPerSample());
}

bool GrainSound::isUsingSameIndex(GrainIndex &ix) const {
  return &ix == index.get();
}

GrainWaveform::Key GrainSound::waveformKeyForGrain(unsigned grain) const {
  return {
      .grain = grain,
      .speedRatio = speedRatio,
      .window = window,
  };
}

GrainSequence::Ptr
GrainSound::grainSequence(const MidiGrainSequence::Midi &midi) {
  return std::make_unique<MidiGrainSequence>(getIndex(), params.sequence, midi);
}

GrainSequence::Ptr GrainSound::grainSequence(unsigned grain, float velocity) {
  auto gainDb = juce::jmap(velocity, params.sequence.gainDbLow,
                           params.sequence.gainDbHigh);
  return std::make_unique<StationaryGrainSequence>(
      getIndex(), GrainSequence::
      Point{grain : grain, gain : juce::Decibels::decibelsToGain(gainDb)});
}

GrainVoice::GrainVoice(GrainData &grainData, const std::mt19937 &prng)
    : grainData(grainData), prng(prng) {}

GrainVoice::~GrainVoice() {}

bool GrainVoice::canPlaySound(juce::SynthesiserSound *sound) {
  return dynamic_cast<GrainSound *>(sound) != nullptr;
}

void GrainVoice::startGrain(unsigned grain, float velocity) {
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound != nullptr) {
    sequence = sound->grainSequence(grain, velocity);
    trimAndRefillQueue(2);
    replaceReservoirWithQueuedGrains();
  }
}

void GrainVoice::clearGrainQueue() {
  sampleOffsetInQueue = 0;
  queue.clear();
}

void GrainVoice::Reservoir::add(const Grain &item) {
  jassert(item.wave != nullptr);
  if (!set.contains(item.seq.grain)) {
    set.add(item.seq.grain);
    grains.push_back(item);
  }
}

void GrainVoice::Reservoir::clear() {
  set.clear();
  grains.clear();
}

void GrainVoice::replaceReservoirWithQueuedGrains() {
  reservoir.clear();
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound != nullptr) {
    for (auto &item : queue) {
      if (item.wave != nullptr) {
        reservoir.add(item);
      }
    }
  }
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
    clearGrainQueue();
    reservoir.clear();
    if (velocity > 0.f) {
      fillQueueForSound(*sound);
      fetchQueueWaveforms(*sound);
    }
  }
}

void GrainVoice::stopNote(float, bool) {
  sequence = nullptr;
  trimQueueToLength(1);
}

bool GrainVoice::isVoiceActive() const { return !queue.empty(); }

void GrainVoice::pitchWheelMoved(int newValue) {
  auto midiSequence = dynamic_cast<MidiGrainSequence *>(sequence.get());
  if (midiSequence != nullptr) {
    midiSequence->midi.pitchWheel = newValue;
    trimAndRefillQueue(2);
  }
}

void GrainVoice::controllerMoved(int controllerNumber, int newValue) {
  if (controllerNumber == 0x01) {
    currentModWheelPosition = newValue;
    auto midiSequence = dynamic_cast<MidiGrainSequence *>(sequence.get());
    if (midiSequence != nullptr) {
      midiSequence->midi.modWheel = newValue;
      trimAndRefillQueue(2);
    }
  }
}

void GrainVoice::renderNextBlock(juce::AudioBuffer<float> &outputBuffer,
                                 int startSample, int numSamples) {
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound == nullptr) {
    return;
  }
  fillQueueForSound(*sound);
  fetchQueueWaveforms(*sound);
  if (queue.empty()) {
    // No more work
    clearCurrentNote();
  } else {
    renderFromQueue(*sound, outputBuffer, startSample, numSamples);
  }
}

void GrainVoice::fillQueueForSound(const GrainSound &sound) {
  if (sequence != nullptr) {
    int target = sound.targetQueueDepth();
    while (queue.size() < target) {
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

void GrainVoice::trimQueueToLength(int length) {
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound == nullptr) {
    queue.clear();
  } else if (queue.size() > length) {
    queue.resize(std::max(length, numActiveGrainsInQueue(*sound)));
  }
}

void GrainVoice::trimAndRefillQueue(int length) {
  trimQueueToLength(length);
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound != nullptr) {
    fillQueueForSound(*sound);
    fetchQueueWaveforms(*sound);
  }
}

void GrainVoice::addListener(Listener *listener) {
  std::lock_guard<std::mutex> guard(listenerMutex);
  listeners.add(listener);
}

void GrainVoice::removeListener(Listener *listener) {
  std::lock_guard<std::mutex> guard(listenerMutex);
  listeners.remove(listener);
}

void GrainVoice::renderFromQueue(const GrainSound &sound,
                                 juce::AudioBuffer<float> &outputBuffer,
                                 int startSample, int numSamples) {
  auto repeatRate = sound.grainRepeatsPerSample();
  int queueTimestamp = 0;
  std::vector<Grain> grainsToRetry;

  for (auto &grain : queue) {
    // If we don't have a waveform loaded yet, save this grain for later
    // and either stall for more time or replace the grain with another.
    if (grain.wave == nullptr) {
      grainsToRetry.push_back(grain);

      if (!reservoir.grains.empty()) {
        // We can replace this grain with one from the Reservoir
        std::uniform_int_distribution<> uniform(0, reservoir.grains.size() - 1);
        grain = reservoir.grains[uniform(prng)];

      } else if (queueTimestamp == 0 && sampleOffsetInQueue == 0) {
        // If we haven't actually started playing yet, we can delay starting
        return;

      } else {
        // We are already playing and there's a missing grain that overlaps with
        // grains we are already playing, so we can't just pause. Silence it.
        auto key = sound.waveformKeyForGrain(grain.seq.grain);
        grain.wave = new GrainWaveform(key, 0, 0);
      }
    }
    if (queueTimestamp > (sampleOffsetInQueue + numSamples)) {
      // Happens after the end of this render block
      break;
    }

    jassert(grain.wave != nullptr);
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

    {
      std::lock_guard<std::mutex> guard(listenerMutex);
      listeners.call(
          [this, &sound, &wave, seq = grain.seq, relative](Listener &l) {
            l.grainVoicePlaying(*this, sound, wave, seq, -relative);
          });
    }

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
    // Done with the grain
    int queueTimestamp = 0;
    if (timestampForNextRepeat(queueTimestamp, repeatRate)) {
      sampleOffsetInQueue -= queueTimestamp;
      // Temporarily hold on to all unique grains on this voice, to
      // use them as replacements for grains that are still loading.
      reservoir.add(queue.front());
      queue.pop_front();
    } else {
      // No repeats, we're entirely done
      queue.clear();
      sequence = nullptr;
    }
  }

  // If we would like to retry grains, requeue them but only if there is
  // spare capacity in the thread pool, it doesn't help to add to a backlog.
  if (!grainsToRetry.empty() && grainData.averageLoadQueueDepth() < 1.f) {
    for (auto &grain : grainsToRetry) {
      queue.push_back(grain);
    }
  }
}
