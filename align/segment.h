#pragma once
#include "pocketsphinx.h"
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

static const arg_t cont_args_def[] = {POCKETSPHINX_OPTIONS, CMDLN_EMPTY_OPTION};

struct SegmentationJob {
  unsigned short surah, ayah;
  std::string in_file;
  std::vector<std::string> in_words;
};

struct RecognizedWord {
  unsigned int start, end;
  std::string text;
};

struct SegmentedWordSpan {
  unsigned int index_start, index_end; // Index within in_string.
  unsigned int start, end;             // Msec within in_file audio.
};

struct SegmentationResult {
  SegmentationResult(SegmentationJob &job, std::vector<SegmentedWordSpan> spans) : job(job), spans(spans){};
  SegmentationJob &job;
  std::vector<SegmentedWordSpan> spans;
};

class SegmentationProcessor {
public:
  SegmentationProcessor(const std::string &cfg_path) : _cfg_path(cfg_path) {};
  ~SegmentationProcessor();
  SegmentationResult Run(SegmentationJob &job, const std::unordered_map<std::string, std::string> &dictionary);

private:
  void ps_setup(const std::unordered_map<std::string, std::string> &dictionary);
  std::string _cfg_path;
  ps_decoder_t *ps = NULL;
};
