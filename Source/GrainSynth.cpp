#include "GrainSynth.h"

GrainSequence::~GrainSequence() {}

float GrainSequence::Params::speedRatio(const GrainIndex &index,
                                        unsigned grain) const {
  return index.sampleRates[grain] / sampleRate * speedWarp;
}

float GrainSequence::Params::maxGrainWidthSamples(
    const GrainIndex &index) const {
  return index.maxGrainWidth * sampleRate / speedWarp;
}

GrainWaveform::Window
GrainSequence::Params::window(const GrainIndex &index) const {
  return GrainWaveform::Window(maxGrainWidthSamples(index), windowParams);
}

unsigned GrainSequence::Params::grain(const GrainIndex &index, float pitch,
                                      float sel) {
  auto bin = index.closestBinForPitch(pitch / speedWarp);
  auto gr = index.grainsForBin(bin);
  auto sel01 = juce::jlimit<float>(0.f, 1.f, std::fmod(sel + 2., 1.));
  return gr.clipValue(gr.getStart() + gr.getLength() * sel01);
}

unsigned GrainSequence::Params::grain(const GrainIndex &index, Rng &rng,
                                      float pitch, float sel) {
  return grain(index, pitchNoise(rng, pitch), selNoise(rng, sel));
}

GrainSequence::Gains
GrainSequence::Params::velocityToGains(Rng &rng, float velocity) const {
  std::uniform_real_distribution<> uniform(-2.f, 2.f);
  float position = stereoCenter + uniform(rng) * stereoSpread;
  float balance = 0.5f + 0.5f * juce::jlimit(-1.f, 1.f, position);
  float gainDb = juce::jmap(velocity, gainDbLow, gainDbHigh);
  float gain = juce::Decibels::decibelsToGain(gainDb);
  return {gain * (1.f - balance), gain * balance};
}

int GrainSequence::Params::samplesUntilNextPoint(Rng &rng) const {
  if (grainRate > 0.f) {
    std::uniform_real_distribution<> uniform(-1.f, 1.f);
    float noise = uniform(rng);
    auto noisyRate =
        std::max(0.01f, grainRate * (1.f + grainRateSpread * noise));
    return std::max<int>(1, sampleRate / noisyRate);
  } else {
    return 0;
  }
}

float GrainSequence::Params::selNoise(Rng &rng, float input) const {
  std::uniform_real_distribution<> uniform(-0.5f, 0.5f);
  return input + selSpread * uniform(rng);
}

float GrainSequence::Params::pitchNoise(Rng &rng, float input) const {
  std::uniform_real_distribution<> uniform(-0.5f, 0.5f);
  float transposeBySemitones = pitchSpread * uniform(rng);
  float pitchRatio = std::pow(2.f, transposeBySemitones / 12.f);
  return input * pitchRatio;
}

TouchGrainSequence::TouchGrainSequence(GrainIndex &index, const Params &params,
                                       const TouchEvent &event)
    : index(index), params(params), event(event) {}

TouchGrainSequence::~TouchGrainSequence() {}

GrainSequence::Point TouchGrainSequence::generate(Rng &rng) {
  auto grain = params.grain(index, rng, event.pitch, event.sel);
  return Point{
      .waveKey =
          {
              .grain = grain,
              .speedRatio = params.speedRatio(index, grain),
              .window = params.window(index),
          },
      .samplesUntilNextPoint = params.samplesUntilNextPoint(rng),
      .gains = params.velocityToGains(rng, event.velocity),
  };
}

MidiGrainSequence::MidiGrainSequence(GrainIndex &index,
                                     const MidiParams &params,
                                     const MidiEvent &event)
    : index(index), params(params), event(event) {}

MidiGrainSequence::~MidiGrainSequence() {}

GrainSequence::Point MidiGrainSequence::generate(Rng &rng) {
  auto bend = params.pitchBendRange * (event.pitchWheel / 8192.0f - 1.0f);
  auto pitch = 440.0f * std::pow(2.0f, (event.note + bend - 69.0f) / 12.0f);
  auto sel =
      params.selCenter + params.selMod * (event.modWheel / 128.0f - 0.5f);
  auto grain = params.common.grain(index, rng, pitch, sel);
  return Point{
      .waveKey =
          {
              .grain = grain,
              .speedRatio = params.common.speedRatio(index, grain),
              .window = params.common.window(index),
          },
      .samplesUntilNextPoint = params.common.samplesUntilNextPoint(rng),
      .gains = params.common.velocityToGains(rng, event.velocity),
  };
}

GrainSynth::GrainSynth(GrainData &grainData, int numVoices) {
  GrainSequence::Rng seedRng;
  for (auto i = 0; i < juce::numElementsInArray(lastModWheelValues); i++) {
    lastModWheelValues[i] = 64;
  }
  for (auto i = 0; i < numVoices; i++) {
    GrainSequence::Rng voiceRng(seedRng());
    addVoice(new GrainVoice(grainData, voiceRng));
  }
}

GrainSynth::~GrainSynth() {}

void GrainSynth::touchEvent(const TouchEvent &event) {
  juce::ScopedLock sl(lock);
  auto sound = dynamic_cast<GrainSound *>(getSound(0).get());
  if (sound == nullptr) {
    return;
  }
  if (event.grain.velocity > 0.f &&
      !voiceForInputSource.contains(event.sourceId)) {
    auto voice = findFreeVoice(getSound(0).get(), -1, -1, true);
    voiceForInputSource.set(event.sourceId, dynamic_cast<GrainVoice *>(voice));
  }
  auto voice = voiceForInputSource[event.sourceId];
  if (voice) {
    if (event.grain.velocity > 0.f) {
      if (!voice->isVoiceActive()) {
        // We don't seem to have a direct way to start a
        // juce::SynthesiserVoice without also triggering a midi note that we
        // need to dequeue.
        startVoice(voice, sound, 0, 0, 0);
        voice->clearGrainQueue();
      }
      voice->startTouch(event.grain);
    } else {
      stopVoice(voice, 0, true);
      voiceForInputSource.remove(event.sourceId);
    }
  }
}

GrainSound::Ptr GrainSynth::latestSound() {
  juce::ScopedLock sl(lock);
  return dynamic_cast<GrainSound *>(sounds[0].get());
}

void GrainSynth::changeSound(GrainIndex &index,
                             const MidiGrainSequence::MidiParams &params) {
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

GrainSound::GrainSound(GrainIndex &index,
                       const MidiGrainSequence::MidiParams &params)
    : index(index), params(params) {}

GrainSound::~GrainSound() {}
bool GrainSound::appliesToNote(int) { return true; }
bool GrainSound::appliesToChannel(int) { return true; }

GrainVoice::GrainVoice(GrainData &grainData, const GrainSequence::Rng &rng)
    : grainData(grainData), rng(rng) {}

GrainVoice::~GrainVoice() {}

bool GrainVoice::canPlaySound(juce::SynthesiserSound *sound) {
  return dynamic_cast<GrainSound *>(sound) != nullptr;
}

void GrainVoice::clearGrainQueue() {
  sampleOffsetInQueue = 0;
  queue.clear();
}

GrainVoice::Reservoir::Reservoir() {}

void GrainVoice::Reservoir::add(const Grain &item) {
  static constexpr int sizeLimit = 32;
  jassert(item.wave != nullptr);
  if (set.find(item.seq.waveKey) == set.end()) {
    set.insert(item.seq.waveKey);
    grains.push_back(item);
    while (grains.size() > sizeLimit) {
      grains.pop_front();
    }
  }
}

void GrainVoice::Reservoir::clear() {
  set.clear();
  grains.clear();
}

bool GrainVoice::Reservoir::empty() const { return grains.empty(); }

const GrainVoice::Grain &
GrainVoice::Reservoir::choose(GrainSequence::Rng &rng) const {
  std::uniform_int_distribution<> uniform(0, grains.size() - 1);
  return grains[uniform(rng)];
}

void GrainVoice::startTouch(const TouchGrainSequence::TouchEvent &event) {
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound != nullptr) {
    sequence = std::make_unique<TouchGrainSequence>(
        *sound->index, sound->params.common, event);
    trimAndRefillQueue(2);
  }
}

void GrainVoice::startNote(int midiNote, float velocity,
                           juce::SynthesiserSound *genericSound,
                           int currentPitchWheelPosition) {
  auto sound = dynamic_cast<GrainSound *>(genericSound);
  if (sound != nullptr) {
    sequence = std::make_unique<MidiGrainSequence>(
        *sound->index, sound->params,
        MidiGrainSequence::MidiEvent{
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
    midiSequence->event.pitchWheel = newValue;
    trimAndRefillQueue(2);
  }
}

void GrainVoice::controllerMoved(int controllerNumber, int newValue) {
  if (controllerNumber == 0x01) {
    currentModWheelPosition = newValue;
    auto midiSequence = dynamic_cast<MidiGrainSequence *>(sequence.get());
    if (midiSequence != nullptr) {
      midiSequence->event.modWheel = newValue;
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
    auto repeatsPerSample =
        sound.params.common.grainRate / sound.params.common.sampleRate;
    int target = std::ceil(
        1. + sound.params.common.window(*sound.index).range().getLength() *
                 repeatsPerSample);
    while (queue.size() < target) {
      queue.push_back({sequence->generate(rng)});
    }
  }
}

void GrainVoice::fetchQueueWaveforms(GrainSound &sound) {
  for (auto &grain : queue) {
    if (grain.wave == nullptr) {
      grain.wave = grainData.getWaveform(*sound.index, grain.seq.waveKey);
      if (grain.wave != nullptr) {
        reservoir.add(grain);
      }
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

int GrainVoice::numActiveGrainsInQueue() {
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

    auto next = grain.seq.samplesUntilNextPoint;
    if (next < 1) {
      break;
    }
    queueTimestamp += next;
  }
  return numActive;
}

void GrainVoice::trimQueueToLength(int length) {
  auto sound = dynamic_cast<GrainSound *>(getCurrentlyPlayingSound().get());
  if (sound == nullptr) {
    queue.clear();
  } else {
    auto deleteAfterLength = std::max(length, numActiveGrainsInQueue());
    while (queue.size() > deleteAfterLength) {
      queue.pop_back();
    }
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
  int queueTimestamp = 0;
  std::vector<Grain> grainsToRetry;

  for (auto &grain : queue) {
    // If we don't have a waveform loaded yet, save this grain for later
    // and either stall for more time or replace the grain with another.
    if (grain.wave == nullptr) {
      grainsToRetry.push_back(grain);

      if (!reservoir.empty()) {
        // We can replace this grain with one from the Reservoir
        grain = reservoir.choose(rng);

      } else if (queueTimestamp == 0 && sampleOffsetInQueue == 0) {
        // If we haven't actually started playing yet, we can delay starting
        return;

      } else {
        // We are already playing and there's a missing grain that overlaps
        // with grains we are already playing, so we can't just pause. Silence
        // it.
        grain.wave = new GrainWaveform(grain.seq.waveKey, 0, 0);
      }
    }
    if (queueTimestamp > (sampleOffsetInQueue + numSamples)) {
      // Happens after the end of this render block
      break;
    }

    jassert(grain.wave != nullptr);
    auto &wave = *grain.wave;
    auto &gains = grain.seq.gains;
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
      auto range = juce::Range<int>::withStartAndLength(-relative, copySize);
      listeners.call(
          [this, &sound, &wave, seq = grain.seq, range](Listener &l) {
            l.grainVoicePlaying(*this, sound, wave, seq, range);
          });
    }

    if (copySize > 0) {
      for (int ch = 0; ch < outChannels; ch++) {
        outputBuffer.addFrom(ch, startSample + copyDest, wave.buffer,
                             ch % inChannels, copySource, copySize,
                             gains[ch % gains.size()]);
      }
    }

    auto next = grain.seq.samplesUntilNextPoint;
    if (next < 1) {
      break;
    }
    queueTimestamp += next;
  }

  // Advance past the rendered block, and remove grains we're fully done with
  sampleOffsetInQueue += numSamples;
  while (!queue.empty()) {
    auto &grain = queue.front();
    if (grain.wave == nullptr) {
      break;
    }
    if (sampleOffsetInQueue < grain.wave->buffer.getNumSamples()) {
      // Still using this grain
      break;
    }
    // Done with the grain
    auto next = grain.seq.samplesUntilNextPoint;
    if (next < 1) {
      // No repeats, we're entirely done
      queue.clear();
      sequence = nullptr;
    } else {
      sampleOffsetInQueue -= next;
      queue.pop_front();
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
