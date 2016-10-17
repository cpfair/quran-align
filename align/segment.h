#pragma once
#include "pocketsphinx.h"
#include <deque>
#include <iostream>
#include <string>
#include <vector>

static const arg_t cont_args_def[] = {POCKETSPHINX_OPTIONS, CMDLN_EMPTY_OPTION};

struct SegmentationJob {
  std::string in_file;
  std::string in_string;
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
  std::vector<SegmentedWordSpan> spans;
};

class SegmentationProcessor {
public:
  SegmentationProcessor(std::string cfg_path);
  ~SegmentationProcessor();
  SegmentationResult Run(SegmentationJob &job);

private:
  cmd_ln_t *ps_opts;
  ps_decoder_t *ps;
};
