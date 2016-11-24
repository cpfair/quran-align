#include "segment.h"
// PROGRAMMER INCLUDES PRIVATE HEADERS, WORLD IN SHOCK - FILM AT 11
#include "fe_internal.h"
#include "pocketsphinx_internal.h"
// WE NOW RETURN TO REGULARLY SCHEDULED PROGRAMMING
#include "debug.h"
#include "discriminator.h"
#include "err.h"
#include "match.h"
#include "pocketsphinx.h"
#include "rates.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stack>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Enforced gap between output words - matches pocketsphinx because I like consistency and 10msec is negligible.
const uint32_t INTERWORD_DELAY = 10; // msec

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

class MMapFile {
public:
  MMapFile(const std::string &filename) {
    struct stat st;
    stat(filename.c_str(), &st);
    _size = st.st_size;
    _fd = open(filename.c_str(), O_RDONLY, 0);
    static const int mmap_flags = MAP_PRIVATE | MAP_POPULATE;
    _data = mmap(NULL, _size, PROT_READ, mmap_flags, _fd, 0);
    // Hope the memory-map op didn't fail.
  }
  ~MMapFile() {
    munmap(_data, _size);
    close(_fd);
  }
  const void *data() { return _data; }
  size_t size() { return _size; }

private:
  int _fd;
  size_t _size;
  void *_data;
};

SegmentationProcessor::SegmentationProcessor(const std::string &ps_cfg) : _cfg_path(ps_cfg) {
  // Load full LM dictionary.
  auto ps_opts = cmd_ln_parse_file_r(NULL, cont_args_def, _cfg_path.c_str(), true);
  std::string line;
  std::ifstream dict_file(cmd_ln_str_r(ps_opts, "-dict"));
  cmd_ln_free_r(ps_opts);
  while (std::getline(dict_file, line)) {
    auto first_space = line.find_first_of(" ");
    if (first_space == std::string::npos) {
      continue;
    }
    auto word = line.substr(0, first_space);
    auto phones = line.substr(first_space + 1);
    _dict[word] = phones;
  }
  dict_file.close();
}

SegmentationProcessor::~SegmentationProcessor() {
  if (ps) {
    ps_free(ps);
  }
  if (fe) {
    fe_free(fe);
  }
}

void SegmentationProcessor::ps_setup(const SegmentationJob &job) {
  // Populate dict.
  std::unordered_map<std::string, std::string> job_dict;
  for (auto word = job.in_words.begin(); word != job.in_words.end(); word++) {
    job_dict[*word] = _dict[*word];
  }
  // Write dictionary to tempfile.
  char dict_fn[L_tmpnam];
  tmpnam(dict_fn);
  std::ofstream dict_fh;
  dict_fh.open(dict_fn);
  for (auto i = job_dict.begin(); i != job_dict.end(); i++) {
    dict_fh << i->first << " " << i->second << std::endl;
  }
  dict_fh.close();

  // Re/Configure PS
  err_set_logfp(NULL);
  err_set_debug_level(0);
  auto ps_opts = cmd_ln_parse_file_r(NULL, cont_args_def, _cfg_path.c_str(), true);
  err_set_logfp(NULL);
  err_set_debug_level(0);
  cmd_ln_set_str_r(ps_opts, "-dict", dict_fn);
  ps_default_search_args(ps_opts);

  if (!ps) {
    ps = ps_init(ps_opts);
  } else {
    ps_reinit(ps, ps_opts);
  }

  if (fe) {
    fe_free(fe);
  }
  fe = fe_init_auto_r(ps_opts);
  err_set_logfp(NULL);
  err_set_debug_level(0);

  unlink((const char *)dict_fn);
}

SegmentationResult SegmentationProcessor::Run(const SegmentationJob &job) {
  ps_setup(job);
  SegmentationResult result(job);
  std::stack<SegmentedWordSpan> run;

  // I have it on good authority that the audio data starts 78 bytes into the
  // file...
  MMapFile audio_file(job.in_file);
  const int16_t *audio_data = (int16_t *)((char *)audio_file.data() + 78);
  size_t audio_samples = (audio_file.size() - 78) / sizeof(int16_t);
  unsigned int audio_len = audio_samples / (WAV_SAMPLE_RATE / 1000); // msec!

  // Make the first SegmentedWordSpan to process.
  run.push({.index_start = 0,
            .index_end = (unsigned int)job.in_words.size(), // One past the last element!
            .start = 0,
            .end = audio_len});

  // Run until we finish all the available work.
  while (!run.empty()) {
    auto span = run.top();
    run.pop();
    // Attempt to further segment this span.
    // Start by running recognition with pocketsphinx.
    std::vector<RecognizedWord> recog_words;
    ps_start_stream(ps);
    ps_start_utt(ps);
    auto frames_processed = ps_process_raw(ps, audio_data + MSEC2WAVF(span.start), MSEC2WAVF(span.end - span.start),
                                           false /* search */, true /* full utterance */);
    if (frames_processed < 0) {
      throw std::runtime_error("Pocketsphinx Fail");
    }
    ps_end_utt(ps);

    auto iter = ps_seg_iter(ps);
    int sil_ct = 0;
    while (iter) {
      uint32_t word_start_frames, word_end_frames;
      ps_seg_frames(iter, (int *)&word_start_frames, (int *)&word_end_frames);
      uint32_t word_start_msec = MFCCF2MSEC(word_start_frames);
      uint32_t word_end_msec = MFCCF2MSEC(word_end_frames);
      auto word_text = ps_seg_word(iter);
      if (strcmp(word_text, "<s>") != 0 && strcmp(word_text, "</s>") != 0 && strcmp(word_text, "<sil>") != 0) {
        DEBUG("Recog " << recog_words.size() << " \"" << word_text << "\" " << word_start_msec << "~" << word_end_msec);
        recog_words.push_back({.start = word_start_msec, .end = word_end_msec, .text = word_text});
      } else if (strcmp(word_text, "</s>") != 0 && recog_words.size() && sil_ct++) {
        // With remove_silence turned off, these are worse than useless and often are reported on top of other reported
        // words?
        DEBUG("SIL " << word_text << " " << word_start_msec << "~" << word_end_msec);
      }
      iter = ps_seg_next(iter);
    }

    // Run matcher against the ayah text and the recognized words.
    // NB since the SegmentedWordSpan can be only part of an ayah, we slice the
    // ayah words vector.
    // And by "slice" I mean "copy while yearning for Go's slicing."
    std::vector<std::string> words_slice(&job.in_words[span.index_start], &job.in_words[span.index_end]);
    auto match_results = match_words(recog_words, words_slice, result.stats);

    // Patch up last word's end time since there's an obscure case where it can be 0.
    if (!match_results.rbegin()->end) {
      match_results.rbegin()->end = audio_len;
    }

    // Drop infeasible spans.
    // This can happen if the qari missed a part of the ayah and the matcher stuffed a bunch of missing words into
    // 10msec.
    match_results.erase(
        std::remove_if(match_results.begin(), match_results.end(),
                       [](SegmentedWordSpan &span) {
                         const uint32_t MIN_WORD_LEN = 100;
                         if (!(span.flags & SpanFlag::MatchedInput)) {
                           if (span.end - span.start < (span.index_end - span.index_start) * MIN_WORD_LEN) {
                             DEBUG("Dropping too-short span " << span.index_start << "-" << span.index_end << " (len "
                                                              << span.end - span.start << ")");
                             return true;
                           }
                         }
                         return false;
                       }),
        match_results.end());

    // Run through discriminator to better resolve inter-word transitions.
    auto aural_silences = discriminate_silence_periods(audio_data, audio_len);
    size_t size_inout = audio_samples;
    auto mfcc = acmod_shim_calculate_mfcc(ps->acmod, audio_data, &size_inout);
    auto aural_transitions = discriminate_transitions(audio_data, mfcc, audio_len);

    for (auto match_res = match_results.begin(); match_res != match_results.end(); match_res++) {
      DEBUG("Match " << match_res->index_start << "-" << match_res->index_end << " " << match_res->start << "~"
                     << match_res->end);
    }
    for (auto sil = aural_silences.begin(); sil != aural_silences.end(); sil++) {
      DEBUG("Silence " << sil->first << "~" << sil->second);
    }
    for (auto tn = aural_transitions.begin(); tn != aural_transitions.end(); tn++) {
      DEBUG("Transition " << *tn);
    }

    // Move any words that fall in silences.
    auto silence_iter = aural_silences.begin();
    for (auto match_res = match_results.begin(); match_res != match_results.end(); match_res++) {
      while (silence_iter != aural_silences.end() && match_res->start > silence_iter->second) {
        silence_iter++;
      }

      if (silence_iter == aural_silences.end()) {
        continue;
      }

      if (match_res->start > silence_iter->first && match_res->start < silence_iter->second) {
        DEBUG("Shifting span " << match_res - match_results.begin() << " start from " << match_res->start
                               << " to end of silence at " << silence_iter->second);
        match_res->start = silence_iter->second;
      }
    }

    // Find pairs of words where the earlier ends with the same letter as the latter starts with.
    for (auto pt = job.liaise_points.begin(); pt != job.liaise_points.end(); pt++) {
      auto match_res = match_results.begin();
      do {
        if (match_res->index_start <= pt->index && match_res->index_end > pt->index) {
          break;
        }
      } while (++match_res != match_results.end());
      if (match_res == match_results.end()) {
        continue;
      }

      auto last_match_res = match_results.begin() + (match_res - match_results.begin() - 1);
      float best_tn = std::numeric_limits<float>::max();
      const float forward_derate = 1; // (Neutered) factor to prefer moving forward rather than backwards...
      const uint32_t max_backtrack = 300;
      for (auto tn = aural_transitions.begin(); tn != aural_transitions.end(); tn++) {
        float derate = *tn > match_res->start ? forward_derate : 1;
        if (std::fabs((float)*tn - (float)match_res->start) * derate <
                std::fabs((float)best_tn - (float)match_res->start) &&
            *tn < match_res->end) {
          if ((int)match_res->start - (int)*tn < (int)max_backtrack) {
            best_tn = *tn;
          }
        } else {
          break;
        }
      }
      if (best_tn < std::numeric_limits<float>::max()) {
        DEBUG("Aur " << best_tn << " span " << pt->index << " running " << match_res->start << "~" << match_res->end);
        if (match_res != match_results.begin()) {
          last_match_res->end = best_tn;
          match_res->start = best_tn + INTERWORD_DELAY;
          DEBUG("Shifting span " << pt->index << " start = " << best_tn + INTERWORD_DELAY << "msec (old " << old_start
                                 << " diff " << INTERWORD_DELAY << ")");
        } else {
          match_res->start = best_tn;
          DEBUG("Shifting span " << pt->index << " start = " << best_tn << "msec");
        }
      }
    }

    // Fix word endings.
    silence_iter = aural_silences.begin();
    for (auto match_res = match_results.begin(); match_res != match_results.end(); match_res++) {
      // Iterate through silences s/t silence_iter is always a silence that ends after the current word.
      while (silence_iter != aural_silences.end() && match_res->end > silence_iter->second) {
        silence_iter++;
      }

      auto next_match_res = match_results.begin() + (match_res - match_results.begin()) + 1;
      if (next_match_res != match_results.end()) {
        // If the silence ends after the current word (see above) and starts before the next word,
        // shift the end of this word forward to the beginning of that silence.
        if (silence_iter != aural_silences.end() && silence_iter->first < next_match_res->start) {
          DEBUG("Shifting end of span " << match_res - match_results.begin() << " to start of silence at "
                                        << silence_iter->first);
          match_res->end = silence_iter->first;
        } else {
          // Otherwise, shift it to immediately before the start of the next word.
          DEBUG("Shifting end of span " << match_res - match_results.begin()
                                        << " to immediately before start of next span at "
                                        << next_match_res->start + INTERWORD_DELAY);
          match_res->end = next_match_res->start - INTERWORD_DELAY;
        }

        // Sanity check
        if (match_res->end < match_res->start) {
          DEBUG("Span " << match_res - match_results.begin() << " ends before it starts!");
        } else if (match_res->end > next_match_res->start) {
          DEBUG("Span " << match_res - match_results.begin() << " starts before the next begins!");
        }
      } else {
        // No next word - we're at the end of an ayah - so snap the word-end to the presumably-final silence.
        if (silence_iter != aural_silences.end()) {
          DEBUG("Shifting end of span " << match_res - match_results.begin() << " to start of final silence at "
                                        << silence_iter->first);
          match_res->end = silence_iter->first;
        }

        // Sanity check, again.
        if (match_res->end < match_res->start) {
          DEBUG("Span " << match_res - match_results.begin() << " ends before it starts!");
        }
      }
    }
    result.spans.swap(match_results);
  }

  return result;
}
