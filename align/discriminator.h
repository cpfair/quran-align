#pragma once
#include <cstdint>
#include <vector>
#include "pocketsphinx.h"


// Return values are pairs of (silence start, silence end) msec timestamps.
std::vector<std::pair<uint32_t, uint32_t>> discriminate_silence_periods(const int16_t* audio, uint32_t length_msec);

// Return values are a msec offset from start_msec.
std::vector<uint32_t> discriminate_transitions(const int16_t* audio, mfcc_t** mfcc, uint32_t length_msec);
