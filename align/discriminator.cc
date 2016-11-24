#include "discriminator.h"
#include <limits>
#include <cmath>
#include <algorithm>
#include "debug.h"

// Some baked-in assumptions about our feature stream.
const size_t MFCC_FRAME_PERIOD = 10; //msec
const size_t AUDIO_SAMPLE_RATE = 16000;
const size_t VECTOR_STRIDE = 13;
const size_t MAX_OFFSET_FRAMES = 2000000; // 2sec
const float VELOCITY_THRESHOLD = 10; // Guesstimated from eyeballing a chart, effectively meaningless.
const size_t POWER_WINDOW = 16 * 50; // 5msec @ 16kHz.
const size_t POWER_WINDOW_STEP = POWER_WINDOW;

std::vector<std::pair<uint32_t, uint32_t>> discriminate_silence_periods(const int16_t* audio, uint32_t length_msec) {
    uint32_t silence_start;
    bool in_silence = false;
    std::vector<std::pair<uint32_t, uint32_t>> results;
    for (unsigned int i = POWER_WINDOW; i < length_msec * (AUDIO_SAMPLE_RATE / 1000); i += POWER_WINDOW_STEP) {
        // RMS power.
        float sum = 0;
        for (int x = -(int)POWER_WINDOW; x < 0; ++x) {
            float val = (float)audio[i + x] / 32768;
            sum += val * val;
        }
        float power = 20 * std::log10(sum / (POWER_WINDOW / 2));
        if (!in_silence && power < -100) {
            in_silence = true;
            silence_start = i / (AUDIO_SAMPLE_RATE / 1000);
        } else if (in_silence && power > -75) {
            in_silence = false;
            results.emplace_back(silence_start, i / (AUDIO_SAMPLE_RATE / 1000));
        }
    }
    return results;
}

static std::vector<size_t> discriminate_transitions_power(const int16_t* audio, size_t len) {
    // We use an online stdev approximation to find peaks within the audio.
    float last_power = 0;
    float mean_power_vel = 0;
    float m2_power_vel = 0;
    int n_samples = 0;
    bool in_peak = false;
    std::vector<size_t> transitions;
    const float vel_cap = 10;
    const float a_mean = 0.99;
    const float a_dev = 0.97;
    for (unsigned int i = POWER_WINDOW + 16 * 30; i < len; i += POWER_WINDOW_STEP) {
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
        if (power < -75) {
            continue;
        }
        if (last_power == 0) {
            last_power = power;
        }
        float vel = std::min(vel_cap, std::abs(power - last_power));
        last_power = power;
        float delta = vel - mean_power_vel;
        mean_power_vel = (mean_power_vel + delta / n_samples) * a_mean + (1 - a_mean) * vel;
        m2_power_vel = (m2_power_vel + delta * (vel - mean_power_vel)) * a_dev;
        if (n_samples > 1) {
            float variance = std::sqrt(m2_power_vel / std::min(100, (n_samples - 1))) * 1.6;
            // std::cerr << i / 16 << "\t" << power << "\t" << vel << "\t" << mean_power_vel << "\t" << mean_power_vel + variance << "\t" << mean_power_vel - variance << std::endl;
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

// My PS models aren't very good at the slurred transitions between some words
// (even when the actual transition point is obvious). So, we look at the velocity of the
// first member in the MF coef vectors, and catch the first time it hits a certain threshold.
static std::vector<size_t> discriminate_transitions_mfcc(mfcc_t** mfcc, size_t len) {
    float mean_vel = 0;
    float m2_vel = 0;
    const float a_mean = 0.95;
    const float a_dev = 0.999;
    const float a_dev_peak = 1;
    std::vector<size_t> transitions;
    // std::cerr << "MFCC GO" << std::endl;
    for (size_t i = 3; i < std::min(MAX_OFFSET_FRAMES, len); ++i) {
        float* last_frame = mfcc[(i - 1)];
        float* this_frame = mfcc[i];
        float vel = 0;
        // std::cerr << i * 10 << "\t";
        for (int x = 0; x < 13; ++x) {
            // std::cerr << this_frame[x] << "\t";
            vel += std::pow((last_frame[x] - this_frame[x]), 2);
        }
        // std::cerr << std::endl;
        vel = std::sqrt(vel);
        float delta = vel - mean_vel;
        bool in_peak = false;
        if (i > 0) {
            float stdev_thresh = std::sqrt(m2_vel / i) * 2.3;
            // std::cerr << i * 10 << "\t" << vel << "\t" << mean_vel << "\t" << mean_vel + stdev_thresh << "\t" << mean_vel - stdev_thresh << std::endl;
            if (vel > stdev_thresh + mean_vel) {
                transitions.push_back(i);
                in_peak = true;
            }
        }
        mean_vel = (mean_vel + delta / std::min((size_t)100, (i + 1))) * a_mean + (1 - a_mean) * vel;
        m2_vel = (m2_vel + delta * (vel - mean_vel) * (in_peak ? a_dev_peak : 1)) * a_dev;
    }
    // std::cerr << "MFCC END" << std::endl;
    return transitions;
}

std::vector<uint32_t> discriminate_transitions(const int16_t* audio, mfcc_t** mfcc, uint32_t length_msec) {
    auto result_mfcc = discriminate_transitions_mfcc(mfcc, length_msec / MFCC_FRAME_PERIOD - 1);
    auto result_power = discriminate_transitions_power(audio, length_msec * (AUDIO_SAMPLE_RATE / 1000) - 1);

    std::vector<uint32_t> transitions_msec;
    auto mfcc_tn_iter = result_mfcc.begin();
    auto power_tn_iter = result_power.begin();
    while (mfcc_tn_iter != result_mfcc.end() && power_tn_iter != result_power.end()) {
        if (mfcc_tn_iter != result_mfcc.end() && *mfcc_tn_iter * MFCC_FRAME_PERIOD < *power_tn_iter / (AUDIO_SAMPLE_RATE / 1000)) {
            transitions_msec.push_back(*(mfcc_tn_iter++) * MFCC_FRAME_PERIOD);
        } else {
            transitions_msec.push_back(*(power_tn_iter++) / (AUDIO_SAMPLE_RATE / 1000));
        }
    }
    return transitions_msec;
}
