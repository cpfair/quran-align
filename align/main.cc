#include "segment.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
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

static void job_executor(std::unordered_map<std::string, std::string>& lm_dict, std::vector<SegmentationJob>& jobs) {
  for (auto job = jobs.begin(); job != jobs.end(); job++) {
    std::cout << "Proc " << job->in_file << std::endl;
    // Populate dict.
    std::unordered_map<std::string, std::string> dict;
    for (auto word = job->in_words.begin(); word != job->in_words.end(); word++) {
      dict[*word] = lm_dict[*word];
    }
    auto seg_proc = new SegmentationProcessor("ps.cfg", dict);
    auto result = seg_proc->Run(*job);
    if (job->in_words.size() != result.spans.size()) {
      std::cout << "Mismatched word count!" << std::endl;
      for (auto i = result.spans.begin(); i != result.spans.end(); i++) {
        std::cout << i->start << "~" << i->end << " words " << i->index_start << "~" << i->index_end << std::endl;
      }
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 4) {
    std::cerr << argv[0] << " quran-uthmani.mod.txt lm.dict ..._sssaaa.wav [..._sssaaa.wav etc.]" << std::endl;
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

  // Load LM dictionary
  std::string line;
  std::ifstream dict_file(argv[2]);
  std::unordered_map<std::string, std::string> lm_dict;
  while (std::getline(dict_file, line)) {
    auto first_space = line.find_first_of(" ");
    if (first_space == std::string::npos) {
      continue;
    }
    auto word = line.substr(0, first_space);
    auto phones = line.substr(first_space);
    lm_dict[word] = phones;
  }
  dict_file.close();

  // Generate jobs and round-robin to worker queues.
  const unsigned int worker_ct = 8;
  std::vector<std::vector<SegmentationJob>> worker_jobs(worker_ct);

  for (int i = 3; i < argc; ++i) {
    if (strlen(argv[i]) < 10 || strcmp(strchr(argv[i], 0) - 4, ".wav") != 0) {
      std::cerr
          << "Input audio filename must end with sssaaa.wav, where sss is the surah number and aaa the ayah number."
          << std::endl;
      exit(1);
    }
    int surah_num = stoi(std::string(argv[i] + strlen(argv[i]) - 10, 3));
    int ayah_num = stoi(std::string(argv[i] + strlen(argv[i]) - 7, 3));
    std::vector<std::string> words;
    std::cout << "Prep " << quran_text[surah_num * 1000 + ayah_num] << std::endl;
    split(quran_text[surah_num * 1000 + ayah_num], ' ', words);
    worker_jobs[i % worker_ct].push_back({argv[i], words});
  }

  // Run jobs.
  std::vector<std::thread> worker_threads;
  for (unsigned int i = 0; i < worker_ct; ++i) {
    worker_threads.emplace_back(
        [&, i] { job_executor(lm_dict, worker_jobs[i]); });
  }
  // Wait for jobs to finish.
  for (unsigned int i = 0; i < worker_ct; ++i) {
    worker_threads[i].join();
  }
}
