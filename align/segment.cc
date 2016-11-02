#include "debug.h"
#include "pocketsphinx.h"
#include "err.h"
#include "segment.h"
#include "match.h"
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <stack>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const unsigned short WAV_SAMPLE_RATE = 16000;
const unsigned char PS_FRAME_RATE = 100; // Msec.

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

SegmentationProcessor::SegmentationProcessor(const std::string& ps_cfg) : _cfg_path(ps_cfg) {
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
    auto phones = line.substr(first_space);
    _dict[word] = phones;
  }
  dict_file.close();
}

SegmentationProcessor::~SegmentationProcessor() {
  if (ps) {
    ps_free(ps);
  }
}

void SegmentationProcessor::ps_setup(const SegmentationJob& job) {
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

  unlink((const char *)dict_fn);
}

SegmentationResult SegmentationProcessor::Run(const SegmentationJob &job) {
  ps_setup(job);
  std::vector<SegmentedWordSpan> result;
  std::stack<SegmentedWordSpan> run;

  // I have it on good authority that the audio data starts 78 bytes into the
  // file...
  MMapFile audio_file(job.in_file);
  int16_t *audio_data = (int16_t *)((char *)audio_file.data() + 78);
  unsigned int audio_len = (audio_file.size() - 78) / sizeof(int16_t) / (WAV_SAMPLE_RATE / 1000); // msec!

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
    int start_msec_off = 0;
    while (true) {
      DEBUG("Recognize from " << start_msec_off);
      ps_start_stream(ps);
      ps_start_utt(ps);
      auto frames_processed = ps_process_raw(ps, audio_data + (start_msec_off + span.start) * (WAV_SAMPLE_RATE / 1000),
                                             (span.end - span.start - start_msec_off) * (WAV_SAMPLE_RATE / 1000), false /* search */,
                                             true /* full utterance */);
      if (frames_processed < 0) {
        throw std::runtime_error("Pocketsphinx Fail");
      }
      ps_end_utt(ps);

      auto iter = ps_seg_iter(ps);
      int sil_ct = 0;
      bool try_again = false;
      while (iter) {
        uint32_t word_start_frames, word_end_frames;
        ps_seg_frames(iter, (int *)&word_start_frames, (int *)&word_end_frames);
        uint32_t word_start_msec = word_start_frames * (1000 / PS_FRAME_RATE) + start_msec_off;
        uint32_t word_end_msec = word_end_frames * (1000 / PS_FRAME_RATE) + start_msec_off;
        auto word_text = ps_seg_word(iter);
        if (strcmp(word_text, "<s>") != 0 && strcmp(word_text, "</s>") != 0 && strcmp(word_text, "<sil>") != 0) {
          DEBUG("Recog " << recog_words.size() << " \"" << word_text << "\" " << word_start_msec << "~" << word_end_msec);
          recog_words.push_back({.start = word_start_msec,
                                 .end = word_end_msec,
                                 .text = word_text});
        } else if (strcmp(word_text, "</s>") != 0 && recog_words.size() && sil_ct++) {
          DEBUG("Restart " << word_text << " " << word_start_msec << "~" << word_end_msec);
          // There's a bug, or at least something that looks like a bug, in PS's VAD in full-utterance processing.
          // Timestamps get progressively more offset for each silence within the utterance.
          // So, we need to re-start recognition after every break in the text.
          // Note we don't bother re-filtering the dictionary here, it might not be worthwhile.
          // We don't restart if this is the first silence in the string.
          start_msec_off = word_end_msec;
          try_again = true;

          // Also, push the end of the last word forward to the start of the silence here.
          recog_words.back().end = word_start_msec;
          break;
        }
        iter = ps_seg_next(iter);
      }
      if (!try_again) {
        break;
      }
    }

    // Run matcher against the ayah text and the recognized words.
    // NB since the SegmentedWordSpan can be only part of an ayah, we slice the
    // ayah words vector.
    // And by "slice" I mean "copy while yearning for Go's slicing."
    std::vector<std::string> words_slice(&job.in_words[span.index_start], &job.in_words[span.index_end]);
    auto match_results = match_words(recog_words, words_slice);
    // TODO: attempt to refine results.
    result = match_results;
  }

  return SegmentationResult(job, result);
}
