#pragma once
// PROGRAMMER INCLUDES PRIVATE HEADERS, WORLD IN SHOCK - FILM AT 11
#include "fe_internal.h"
#include "pocketsphinx_internal.h"
// WE NOW RETURN TO REGULARLY SCHEDULED PROGRAMMING
#include "pocketsphinx.h"

mfcc_t **acmod_shim_calculate_mfcc(acmod_t *acmod, int16 const *audio_data, size_t *inout_n_samps);
