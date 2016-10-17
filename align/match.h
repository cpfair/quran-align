#pragma once
#include "segment.h"
std::vector<SegmentedWordSpan> match_words(std::vector<RecognizedWord> &input_words,
                                           std::vector<std::string> &reference_words);
