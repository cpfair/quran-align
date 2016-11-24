CMUSPHINX_ROOT = ../../cmusphinx/
CC = gcc
CFLAGS = --std=c++11 -ggdb -O0 -Wall `pkg-config --cflags sphinxbase pocketsphinx`
CFLAGS += -I$(CMUSPHINX_ROOT)pocketsphinx-5prealpha/src/libpocketsphinx/
CFLAGS += -I$(CMUSPHINX_ROOT)sphinxbase-5prealpha/src/libsphinxbase/fe/
LDFLAGS = `pkg-config --libs sphinxbase pocketsphinx` -lstdc++

all: main.cc segment.cc
	$(CC) $(CFLAGS) main.cc segment.cc match.cc discriminator.cc -o align $(LDFLAGS)

clean:
	rm -rf align