#include "debug.h"
#include "segment.h"
#include "vendor/json.hpp"
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <unordered_map>

// http://stackoverflow.com/a/236803
static void split(const std::string &s, char delim, std::vector<std::string> &elems) {
  std::stringstream ss;
  ss.str(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    elems.push_back(item);
  }
}

static void collapse_muqataat(SegmentationResult &result) {
  // Muqata'at are represented in the recognition model as discrete words.
  // This has turned out to be an inconvenient decision, regardless of its merits.
  // We collapse them back into a single word here, triggered by the fact they are
  // of form  __...__ in the reference text.
  std::vector<SegmentedWordSpan> original_spans;
  original_spans.swap(result.spans);
  int collapsed_muqataat = 0;
  for (auto span = original_spans.begin(); span != original_spans.end(); span++) {
    if ((collapsed_muqataat && result.job.in_words[span->index_start].size() == 0) ||
        result.job.in_words[span->index_start][0] == '_') {
      if (!collapsed_muqataat) {
        span->index_end = 1;
        result.spans.push_back(*span);
      }
      result.spans.back().end = span->end;
      collapsed_muqataat++;
    } else {
      if (collapsed_muqataat) {
        span->index_start -= collapsed_muqataat - 1;
        span->index_end -= collapsed_muqataat - 1;
      }
      result.spans.push_back(*span);
    }
  }
}

static void job_executor(std::string ps_cfg, std::queue<SegmentationJob *> &jobs, std::mutex &jobs_mtx,
                         std::vector<SegmentationResult> &results) {
  SegmentationProcessor seg_proc(ps_cfg);
  while (true) {
    std::unique_lock<std::mutex> jobs_lock(jobs_mtx);
    if (jobs.empty()) {
      return;
    }
    auto job = jobs.front();
    jobs.pop();
    jobs_lock.unlock();
    DEBUG("Proc " << job->in_file);
    auto result = seg_proc.Run(*job);
    collapse_muqataat(result);
    results.push_back(result);
    if (job->in_words.size() != result.spans.size()) {
      DEBUG("Mismatched word count! Ref " << job->in_words.size() << " matched " << result.spans.size() << " spans");
      for (auto i = result.spans.begin(); i != result.spans.end(); i++) {
        DEBUG(i->start << "~" << i->end << " words " << i->index_start << "~" << i->index_end);
      }
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 5) {
    std::cerr << argv[0] << " quran.txt quran.liaise.txt ps.cfg ..._sssaaa.wav [..._sssaaa.wav etc.]" << std::endl;
    std::cerr << "  quran.txt is the input used to generate the recognition LM (Tanzil.net format)" << std::endl;
    std::cerr << "  quran.liaise.txt is the list of surah-ayah-wordindex-flags that require transition "
                 "discrimination (set flags field to 1 to start)";
    std::cerr << "  ps.cfg is the full phonetic dictionary from said LM, used in training the AM" << std::endl;
    std::cerr << "  .wav files are EveryAyah recitation audio clips" << std::endl;
    std::cerr << std::endl << "Output is JSON. Each member of `segments` is a tuple:" << std::endl;
    std::cerr << "  (start word index, end word index, start time msec, end time msec)" << std::endl;
    std::cerr << "  Segments may contain multiple words. Indexes are on splitting input text by spaces." << std::endl;
    exit(1);
  }

  // Load Quran text file.
  // (Tanzil.net format)
  std::ifstream quran_file(argv[1]);
  std::unordered_map<unsigned int, std::string> quran_text;
  std::string value;
  while (quran_file.good()) {
    std::getline(quran_file, value, '|');
    if (!value.length()) {
      continue;
    }
    if (value[0] == '#') {
      std::getline(quran_file, value);
      continue;
    }
    int surah_ayah_key = stoi(value) * 1000;
    std::getline(quran_file, value, '|');
    surah_ayah_key += stoi(value);
    std::getline(quran_file, value, '\n');
    quran_text[surah_ayah_key] = value;
  }
  quran_file.close();

  // Load liaise-point definitions.
  std::ifstream quran_liaise_file(argv[2]);
  std::unordered_map<unsigned int, std::vector<LiaisePoint>> liaise_points;
  while (quran_liaise_file.good()) {
    uint16_t surah_num, ayah_num, word, flags;
    quran_liaise_file >> surah_num;
    quran_liaise_file >> ayah_num;
    quran_liaise_file >> word;
    quran_liaise_file >> flags;
    liaise_points[surah_num * 1000 + ayah_num].push_back({word, (LiaiseFlags)flags});
  }

  // Generate jobs and round-robin to worker queues.
  const unsigned int worker_ct = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 4;
  // Jobs need to survive after they're popped from the queue.
  // (since the Result has a ref to it - meh).
  std::vector<SegmentationJob> jobs;
  std::queue<SegmentationJob *> job_queue;
  std::vector<std::vector<SegmentationResult>> worker_results(worker_ct);
  for (int i = 4; i < argc; ++i) {
    if (strlen(argv[i]) < 10 || strcmp(strchr(argv[i], 0) - 4, ".wav") != 0) {
      std::cerr
          << "Input audio filename must end with sssaaa.wav, where sss is the surah number and aaa the ayah number."
          << std::endl;
      exit(1);
    }
    unsigned short surah_num = stoi(std::string(argv[i] + strlen(argv[i]) - 10, 3));
    unsigned short ayah_num = stoi(std::string(argv[i] + strlen(argv[i]) - 7, 3));
    std::vector<std::string> words;
    DEBUG("Prep " << quran_text[surah_num * 1000 + ayah_num]);
    split(quran_text[surah_num * 1000 + ayah_num], ' ', words);
    jobs.push_back({surah_num, ayah_num, argv[i], words, liaise_points[surah_num * 1000 + ayah_num]});
  }

  // Fill job queue.
  for (auto job = jobs.begin(); job != jobs.end(); job++) {
    job_queue.push(&(*job));
  }

  // Run jobs.
  const std::time_t start_time = time(NULL);
  std::mutex jobs_mtx;
  std::vector<std::thread> worker_threads;
  for (unsigned int i = 0; i < worker_ct; ++i) {
    worker_threads.emplace_back([&, i] { job_executor(argv[3], job_queue, jobs_mtx, worker_results[i]); });
  }
  // Spin and display progress.
  do {
    unsigned int elapsed_seconds = time(NULL) - start_time;
    int completed_jobs = jobs.size() - job_queue.size();
    float jobs_per_second = elapsed_seconds ? (float)completed_jobs / (float)elapsed_seconds : 9999;
    unsigned int secs_remaining = (float)job_queue.size() / jobs_per_second;
    std::cerr << "\33[2K\rDone " << completed_jobs << "/" << jobs.size() << " ayah (" << elapsed_seconds
              << " seconds elapsed, " << secs_remaining << " to go)";
    sleep(1);
  } while (job_queue.size());
  std::cerr << std::endl << "Waiting for last jobs to finish..." << std::endl;
  // Wait for jobs to really finish.
  for (unsigned int i = 0; i < worker_ct; ++i) {
    worker_threads[i].join();
  }

  // Serialize results to JSON.
  nlohmann::json results_json;
  for (unsigned int i = 0; i < worker_ct; ++i) {
    for (auto result = worker_results[i].begin(); result != worker_results[i].end(); result++) {
      nlohmann::json result_json;
      result_json["surah"] = result->job.surah;
      result_json["ayah"] = result->job.ayah;
      result_json["stats"] = nlohmann::json();
      result_json["stats"]["insertions"] = result->stats.insertions;
      result_json["stats"]["deletions"] = result->stats.deletions;
      result_json["stats"]["transpositions"] = result->stats.transpositions;
      for (auto span = result->spans.begin(); span != result->spans.end(); span++) {
        result_json["segments"].push_back({span->index_start, span->index_end, span->start, span->end});
      }
      results_json.push_back(result_json);
    }
  }

  std::cout << results_json;
  return 0;
}
