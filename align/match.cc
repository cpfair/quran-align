#include "match.h"

static const unsigned int NO_MATCH = ~0;
#define IDX(i, j) ((i)*mtx_stride + (j))
enum Pick : uint8_t { I = 1, J = 2, Both = 3 };

struct AlignedWord {
  RecognizedWord *input_word;
  unsigned int reference_index;
};

// This is the standard DP alignment algorithm.
static std::vector<AlignedWord> align_words(std::vector<RecognizedWord> &input_words,
                                            std::vector<std::string> &reference_words) {
  const size_t mtx_rows = input_words.size() + 1;
  const size_t mtx_stride = reference_words.size() + 1;
  std::vector<uint16_t> cost_mtx(mtx_rows * mtx_stride);
  std::vector<Pick> back_mtx(mtx_rows * mtx_stride);

  // Initialize matrices.
  for (unsigned int i = 0; i <= input_words.size(); ++i) {
    cost_mtx[IDX(i, 0)] = i;
    back_mtx[IDX(i, 0)] = Pick::I;
  }
  for (unsigned int j = 0; j <= reference_words.size(); ++j) {
    cost_mtx[IDX(0, j)] = j;
    back_mtx[IDX(0, j)] = Pick::J;
  }

  // Populate matrices.
  uint16_t this_cost, cost_both, cost_i, cost_j;
  for (unsigned int i = 1; i <= input_words.size(); ++i) {
    for (unsigned int j = 1; j <= reference_words.size(); ++j) {
      if (input_words[i - 1].text == reference_words[j - 1]) {
        this_cost = 0;
      } else {
        this_cost = 1;
      }
      cost_both = cost_mtx[IDX(i - 1, j - 1)] + this_cost;
      cost_i = cost_mtx[IDX(i - 1, j)] + this_cost;
      cost_j = cost_mtx[IDX(i, j - 1)] + this_cost;
      if (cost_both <= cost_i && cost_both <= cost_j) {
        back_mtx[IDX(i, j)] = Pick::Both;
        cost_mtx[IDX(i, j)] = cost_both;
      } else if (cost_i <= cost_both && cost_i <= cost_j) {
        back_mtx[IDX(i, j)] = Pick::I;
        cost_mtx[IDX(i, j)] = cost_i;
      } else {
        back_mtx[IDX(i, j)] = Pick::J;
        cost_mtx[IDX(i, j)] = cost_j;
      }
    }
  }

  // for (unsigned int j = 0; j <= reference_words.size(); ++j) {
  //   for (unsigned int i = 0; i <= input_words.size(); ++i) {
  //     std::cout << cost_mtx[IDX(i, j)] << "\t";
  //   }
  //   std::cout << std::endl;
  // }

  // Backtrace to build aligned sequence.
  std::vector<AlignedWord> result;
  unsigned int i = input_words.size(), j = reference_words.size();
  while (i != 0 && j != 0) {
    switch (back_mtx[IDX(i, j)]) {
    case Pick::Both:
      i--;
      j--;
      result.insert(result.begin(), {&input_words[i], j});
      break;
    case Pick::I:
      i--;
      result.insert(result.begin(), {&input_words[i], NO_MATCH});
      break;
    case Pick::J:
      j--;
      result.insert(result.begin(), {NULL, j});
      break;
    }
  }
  return result;
}

std::vector<SegmentedWordSpan> match_words(std::vector<RecognizedWord> &input_words,
                                           std::vector<std::string> &reference_words) {
  auto align_result = align_words(input_words, reference_words);
  std::vector<SegmentedWordSpan> result;

  SegmentedWordSpan run_span;
  bool in_run_span = false;
  for (auto i = align_result.begin(); i != align_result.end(); ++i) {
    if (i->input_word) {
      std::cout << "Input " << i->input_word->text << " (" << i->input_word->start << "~" << i->input_word->end << ") match " << i->reference_index << std::endl;
    } else {
      std::cout << "Input ??? match " << i->reference_index << std::endl;
    }
    if (i->input_word != NULL && i->reference_index != NO_MATCH &&
        i->input_word->text == reference_words[i->reference_index]) {
      std::cout << " (exact)" << std::endl;
      // Exact match.
      // First, close existing span.
      if (in_run_span) {
        if (run_span.index_end > run_span.index_start) {
          run_span.end = i->input_word->end;
          result.push_back(run_span);
        }
        in_run_span = false;
      }
      // Insert a fresh span just for this word.
      result.push_back({.index_start = i->reference_index,
                        .index_end = i->reference_index + 1,
                        .start = i->input_word->start,
                        .end = i->input_word->end});
    } else if (i->input_word != NULL && i->reference_index != NO_MATCH) {
      // Inexact match.
      // Start a new span (if reqd.) then add this word to it.
      std::cout << "  (inexact0 - " << run_span.index_start << "~" << run_span.index_end << ")" << std::endl;
      if (!in_run_span) {
        in_run_span = true;
        run_span.index_start = i->reference_index;
        run_span.start = i->input_word->start;
      } else if (run_span.index_start == NO_MATCH) {
        run_span.index_start = i->reference_index;
      }
      run_span.index_end = i->reference_index + 1;
      run_span.end = i->input_word->end;
      std::cout << "  (inexact - " << run_span.index_start << "~" << run_span.index_end << ")" << std::endl;
    } else if (i->input_word == NULL) {
      // Missing word from input.
      // Start a new span (if reqd.), starting from the end of the previous if
      // available.
      // Then add this word to it.
      // The span will close once we regain sync.
      if (!in_run_span) {
        in_run_span = true;
        run_span.index_start = i->reference_index;
        if (!result.empty()) {
          run_span.start = result.back().end;
        }
      }
      run_span.index_end = i->reference_index + 1;
    } else if (i->reference_index == NO_MATCH) {
      // Additional word in input.
      // Start a new span (if reqd. - for the sake of timestamps) but don't add
      // any word to it.
      if (!in_run_span) {
        in_run_span = true;
        run_span.start = i->input_word->start;
        run_span.index_start = run_span.index_end = NO_MATCH;
        std::cout << "  (start)" << std::endl;
      }
      std::cout << "  (spurious - " << run_span.index_start << "~" << run_span.index_end << ")" << std::endl;
    }
  }

  // Close any worthwhile span still open.
  if (in_run_span && run_span.index_end > run_span.index_start) {
    result.push_back(run_span);
  }
  return result;
}
