#include "ps_shim.h"

// Shamelessly copy-pasted from pocketsphinx source code...
mfcc_t **acmod_shim_calculate_mfcc(acmod_t *acmod, int16 const *audio_data, size_t *inout_n_samps) {
  int32 nfr, ntail;

  /* Resize mfc_buf to fit. */
  if (fe_process_frames(acmod->fe, NULL, inout_n_samps, NULL, &nfr, NULL) < 0) {
    return NULL;
  }
  if (acmod->n_mfc_alloc < nfr + 1) {
    ckd_free_2d(acmod->mfc_buf);
    acmod->mfc_buf = (mfcc_t **)ckd_calloc_2d(nfr + 1, fe_get_output_size(acmod->fe), sizeof(**acmod->mfc_buf));
    acmod->n_mfc_alloc = nfr + 1;
  }
  acmod->n_mfc_frame = 0;
  acmod->mfc_outidx = 0;
  auto old_xform = acmod->fe->transform;
  acmod->fe->transform = LEGACY_DCT;
  fe_start_stream(acmod->fe);
  fe_start_utt(acmod->fe);

  if (fe_process_frames(acmod->fe, &audio_data, inout_n_samps, acmod->mfc_buf, &nfr, NULL) < 0) {
    return NULL;
  }
  fe_end_utt(acmod->fe, acmod->mfc_buf[nfr], &ntail);
  nfr += ntail;
  *inout_n_samps = nfr;
  acmod->fe->transform = old_xform;
  return acmod->mfc_buf;
}
