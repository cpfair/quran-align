#include "segment.h"
#include <fstream>
#include <iostream>
#include <unordered_map>

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cerr << argv[0] << " quran-uthmani.mod.txt ..._sssaaa.wav [..._sssaaa.wav etc.]" << std::endl;
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

  // Generate jobs.
  std::deque<SegmentationJob> jobs;
  for (int i = 2; i < argc; ++i) {
    if (strlen(argv[i]) < 10 || strcmp(strchr(argv[i], 0) - 4, ".wav") != 0) {
      std::cerr
          << "Input audio filename must end with sssaaa.wav, where sss is the surah number and aaa the ayah number."
          << std::endl;
      exit(1);
    }
    int surah_num = stoi(std::string(argv[i] + strlen(argv[i]) - 10, 3));
    int ayah_num = stoi(std::string(argv[i] + strlen(argv[i]) - 7, 3));
    jobs.push_back({argv[i], quran_text[surah_num * 1000 + ayah_num]});
  }

  // Run jobs (TODO obviously in many threads at some point).
  for (auto job = jobs.begin(); job != jobs.end(); job++) {
    std::cout << "Proc " << job->in_file << " against \"" << job->in_string << "\"" << std::endl;
    auto seg_proc = new SegmentationProcessor("ps.cfg");
    auto result = seg_proc->Run(*job);
    std::cout << "Results:" << std::endl;
    for (auto i = result.spans.begin(); i != result.spans.end(); i++) {
      std::cout << i->start << "~" << i->end << " words " << i->index_start << "~" << i->index_end << std::endl;
    }
  }
}
