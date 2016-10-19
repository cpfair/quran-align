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

SegmentationProcessor::SegmentationProcessor(const std::string &cfg_path, const std::unordered_map<std::string, std::string> &dictionary) {
  // Write dictionary to tempfile.
  tmpnam((char *)_dict_fn);
  std::ofstream dict_fh;
  dict_fh.open((const char *)_dict_fn);
  for (auto i = dictionary.begin(); i != dictionary.end(); i++) {
    dict_fh << i->first << " " << i->second << std::endl;
  }
  dict_fh.close();

  ps_opts = cmd_ln_parse_file_r(NULL, cont_args_def, cfg_path.c_str(), true);
  cmd_ln_set_str_r(ps_opts, "-dict", (const char *)_dict_fn);
  ps_default_search_args(ps_opts);
  ps = ps_init(ps_opts);
}

SegmentationProcessor::~SegmentationProcessor() {
  ps_free(ps);
  cmd_ln_free_r(ps_opts);
  unlink((const char *)_dict_fn);
}

SegmentationResult SegmentationProcessor::Run(SegmentationJob &job) {
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
    ps_start_utt(ps);
    auto frames_processed = ps_process_raw(ps, audio_data + span.start * (WAV_SAMPLE_RATE / 1000),
                                           (span.end - span.start) * (WAV_SAMPLE_RATE / 1000), false /* search */,
                                           true /* full utterance */);
    if (frames_processed < 0) {
      throw std::runtime_error("Pocketsphinx Fail");
    }
    ps_end_utt(ps);

    std::vector<RecognizedWord> recog_words;
    auto iter = ps_seg_iter(ps);
    while (iter) {
      uint32_t word_start_frames, word_end_frames;
      ps_seg_frames(iter, (int *)&word_start_frames, (int *)&word_end_frames);
      auto word_text = ps_seg_word(iter);
      if (strcmp(word_text, "<s>") != 0 && strcmp(word_text, "</s>") != 0 && strcmp(word_text, "<sil>") != 0) {
        std::cout << "Recog \"" << word_text << "\"" << std::endl;
        recog_words.push_back({.start = word_start_frames * (1000 / PS_FRAME_RATE),
                               .end = word_end_frames * (1000 / PS_FRAME_RATE),
                               .text = word_text});
      } else {
        std::cout << "Skip " << word_text << std::endl;
      }
      iter = ps_seg_next(iter);
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

  return SegmentationResult{.spans = result};
}
