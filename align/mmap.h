#pragma once
#include <string>
#include <unistd.h>

class MMapFile {
public:
  MMapFile(const std::string &filename);
  ~MMapFile();
  const void *data() { return _data; }
  size_t size() { return _size; }

private:
  int _fd;
  size_t _size;
  void *_data;
};
