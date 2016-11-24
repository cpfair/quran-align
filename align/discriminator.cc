#include "discriminator.h"
#include "debug.h"
#include <algorithm>
#include <cmath>
#include <limits>

// #define DUMP_STREAM(...) std::cerr << __VA_ARGS__ << std::endl
#define DUMP_STREAM(...)

// Some baked-in assumptions about our input streams.
const size_t MFCC_FRAME_PERIOD = 10;  // msec
const size_t WAV_SAMPLE_RATE = 16000; // Hz
#define MFCCF2MSEC(mfcc_frame) (mfcc_frame * MFCC_FRAME_PERIOD)
#define MSEC2MFCCF(msec) (msec / MFCC_FRAME_PERIOD)
#define WAVF2MSEC(wav_frame) (wav_frame / (WAV_SAMPLE_RATE / 1000))
#define MSEC2WAVF(msec) (msec * (WAV_SAMPLE_RATE / 1000))

// How many elements are in each MFCC vector.
const size_t VECTOR_STRIDE = 13;
// Controls the window size for power calculations.
const size_t POWER_WINDOW = MSEC2WAVF(50);
const size_t POWER_WINDOW_STEP = POWER_WINDOW;
const float POWER_SILENCE_START = -100; // A silence starts at this power, dbFS...
const float POWER_SILENCE_END = -75;    // ...and ends at this, also dbFS.

std::vector<std::pair<uint32_t, uint32_t>> discriminate_silence_periods(const int16_t *audio, uint32_t length_msec) {
  // No explicit debouncing, but our hysteresis range is fairly large.
  uint32_t silence_start;
  bool in_silence = false;
  std::vector<std::pair<uint32_t, uint32_t>> results;
  for (unsigned int frame = POWER_WINDOW; frame < MSEC2WAVF(length_msec); frame += POWER_WINDOW_STEP) {
    // RMS power.
    float sum = 0;
    for (int x = -(int)POWER_WINDOW; x < 0; ++x) {
      float val = (float)audio[frame + x] / 32768;
      sum += val * val;
    }
    float power = 20 * std::log10(sum / (POWER_WINDOW / 2));
    if (!in_silence && power < POWER_SILENCE_START) {
      in_silence = true;
      silence_start = WAVF2MSEC(frame);
    } else if (in_silence && power > POWER_SILENCE_END) {
      in_silence = false;
      results.emplace_back(silence_start, WAVF2MSEC(frame));
    }
  }
  return results;
}

static std::vector<size_t> discriminate_transitions_power(const int16_t *audio, size_t len) {
  const float POWER_VEL_CAP = 10;
  // We use an online stdev approximation to find peaks within the audio.
  // Decay factors for mean and variance values:
  const float A_MEAN = 0.99;
  const float A_VAR = 0.97;
  // Cap for sample count used in online variance calculation, to account for the exponential running average configured
  // above.
  const int VAR_MIX_CAP = 100;
  // Empirically determined multiplier - power velocity higher than this factor times the current stdev triggers the
  // detector.
  const float THRESH_SIGMA = 1.6;
  // Skip this many frames at the start - one of those things I don't think is actually needed but am scared to remove.
  const int SKIP_LEAD = 30;

  float last_power = 0;
  float mean_power_vel = 0;
  float m2_power_vel = 0;
  int n_samples = 0;
  bool in_peak = false;
  std::vector<size_t> transitions;
  for (unsigned int i = POWER_WINDOW + MSEC2WAVF(SKIP_LEAD); i < len; i += POWER_WINDOW_STEP) {
    // RMS power.
    float sum = 0;
    for (int x = -(int)POWER_WINDOW; x < 0; ++x) {
      float val = (float)audio[i + x] / 32768;
      sum += val * val;
    }
    if (sum == 0) {
      continue;
    }
    n_samples++;
    float power = 20 * std::log10(sum / (POWER_WINDOW / 2));
    if (power < POWER_SILENCE_END) {
      // Drop silent frames - they can't get up to any good.
      continue;
    }
    if (last_power == 0) {
      last_power = power;
    }
    float vel = std::min(POWER_VEL_CAP, std::abs(power - last_power));
    last_power = power;
    float delta = vel - mean_power_vel;
    mean_power_vel = (mean_power_vel + delta / n_samples) * A_MEAN + (1 - A_MEAN) * vel;
    m2_power_vel = (m2_power_vel + delta * (vel - mean_power_vel)) * A_VAR;
    if (n_samples > 1) {
      float variance = std::sqrt(m2_power_vel / std::min(VAR_MIX_CAP, (n_samples - 1))) * THRESH_SIGMA;
      if (vel > mean_power_vel + variance) {
        if (!in_peak) {
          transitions.push_back(i - POWER_WINDOW);
        }
        in_peak = true;
      } else {
        in_peak = false;
      }
    }
  }
  return transitions;
}

static std::vector<size_t> discriminate_transitions_mfcc(mfcc_t **mfcc, size_t len) {
  // As above.
  const float A_MEAN = 0.95;
  const float A_VAR = 0.999;
  const float A_VAR_IN_PEAK = 1;
  const int VAR_MIX_CAP = 100;
  const float THRESH_SIGMA = 2.3;

  float mean_vel = 0;
  float m2_vel = 0;
  std::vector<size_t> transitions;
  DUMP_STREAM("MFCC GO");
  for (size_t i = 3; i < len; ++i) {
    float *last_frame = mfcc[(i - 1)];
    float *this_frame = mfcc[i];
    float vel = 0;
    for (size_t x = 0; x < VECTOR_STRIDE; ++x) {
      vel += std::pow((last_frame[x] - this_frame[x]), 2);
    }
    vel = std::sqrt(vel);
    float delta = vel - mean_vel;
    bool in_peak = false;
    if (i > 0) {
      float stdev_thresh = std::sqrt(m2_vel / i) * THRESH_SIGMA;
      DUMP_STREAM(MFCCF2MSEC(i) << "\t" << vel << "\t" << mean_vel << "\t" << mean_vel + stdev_thresh << "\t"
                                << mean_vel - stdev_thresh);
      if (vel > stdev_thresh + mean_vel) {
        transitions.push_back(i);
        in_peak = true;
      }
    }
    mean_vel = (mean_vel + delta / std::min((size_t)VAR_MIX_CAP, (i + 1))) * A_MEAN + (1 - A_MEAN) * vel;
    m2_vel = (m2_vel + delta * (vel - mean_vel) * (in_peak ? A_VAR_IN_PEAK : 1)) * A_VAR;
  }
  DUMP_STREAM("MFCC END");
  return transitions;
}

std::vector<uint32_t> discriminate_transitions(const int16_t *audio, mfcc_t **mfcc, uint32_t length_msec) {
  auto result_mfcc = discriminate_transitions_mfcc(mfcc, MSEC2MFCCF(length_msec) - 1);
  auto result_power = discriminate_transitions_power(audio, MSEC2WAVF(length_msec) - 1);

  // Interleave the two result sequences chronologically.
  // We treat them equivalently after this point.
  std::vector<uint32_t> transitions_msec;
  auto mfcc_tn_iter = result_mfcc.begin();
  auto power_tn_iter = result_power.begin();
  while (mfcc_tn_iter != result_mfcc.end() && power_tn_iter != result_power.end()) {
    if (mfcc_tn_iter != result_mfcc.end() && MFCCF2MSEC(*mfcc_tn_iter) < WAVF2MSEC(*power_tn_iter)) {
      transitions_msec.push_back(MFCCF2MSEC(*(mfcc_tn_iter++)));
    } else {
      transitions_msec.push_back(WAVF2MSEC(*(power_tn_iter++)));
    }
  }
  return transitions_msec;
}
