#include "mmap.h"
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

MMapFile::MMapFile(const std::string &filename) {
  struct stat st;
  stat(filename.c_str(), &st);
  _size = st.st_size;
  _fd = open(filename.c_str(), O_RDONLY, 0);
  static const int mmap_flags = MAP_PRIVATE | MAP_POPULATE;
  _data = mmap(NULL, _size, PROT_READ, mmap_flags, _fd, 0);
  // Hope the memory-map op didn't fail.
}

MMapFile::~MMapFile() {
  munmap(_data, _size);
  close(_fd);
}
