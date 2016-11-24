#pragma once
#include "pocketsphinx.h"
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <set>

static const arg_t cont_args_def[] = {POCKETSPHINX_OPTIONS, CMDLN_EMPTY_OPTION};

enum LiaiseFlags {
  Backtrack = 1
};

struct LiaisePoint {
  uint16_t index;
  LiaiseFlags flags;
};

struct SegmentationJob {
  unsigned short surah, ayah;
  std::string in_file;
  std::vector<std::string> in_words;
  std::vector<LiaisePoint> liase_points;
};

struct RecognizedWord {
  unsigned int start, end;
  std::string text;
};

enum SpanFlag {
  Clear = 0,
  MatchedInput = 1,
  MatchedReference = 2,
  Exact = 4,
  Inexact = 8
};

struct SegmentedWordSpan {
  unsigned int index_start, index_end; // Index within in_string.
  unsigned int start, end;             // Msec within in_file audio.
  SpanFlag flags;                      // Provenance info for the wordspan.
};

struct SegmentationStats {
  size_t insertions = 0;
  size_t deletions = 0;
  size_t transpositions = 0;
};

struct SegmentationResult {
  SegmentationResult(const SegmentationJob &job) : job(job) {};
  const SegmentationJob &job;
  std::vector<SegmentedWordSpan> spans;
  SegmentationStats stats;
};

class SegmentationProcessor {
public:
  SegmentationProcessor(const std::string &cfg_path);
  ~SegmentationProcessor();
  SegmentationResult Run(const SegmentationJob &job);

private:
  void ps_setup(const SegmentationJob& job);
  std::string _cfg_path;
  std::unordered_map<std::string, std::string> _dict;
  ps_decoder_t *ps = NULL;
  fe_t *fe = NULL;
};
