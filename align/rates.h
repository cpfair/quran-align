#pragma once

// Some baked-in assumptions about our input streams.
#define MFCC_FRAME_PERIOD 10
#define WAV_SAMPLE_RATE 16000
#define MFCCF2MSEC(mfcc_frame) (mfcc_frame * MFCC_FRAME_PERIOD)
#define MSEC2MFCCF(msec) (msec / MFCC_FRAME_PERIOD)
#define WAVF2MSEC(wav_frame) (wav_frame / (WAV_SAMPLE_RATE / 1000))
#define MSEC2WAVF(msec) (msec * (WAV_SAMPLE_RATE / 1000))
