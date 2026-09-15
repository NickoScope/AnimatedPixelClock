# Host build of the firmware's audio DSP, held to tools/audiofx/dsp.py on the same WAVs.
#   make -C tools/audiofx/host check
SRC      := ../../../src/audio
CXX      ?= c++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -I$(SRC)

build/test_dsp: test_dsp.cpp $(SRC)/audio_dsp.cpp $(SRC)/audio_dsp.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test_dsp.cpp $(SRC)/audio_dsp.cpp

check: build/test_dsp
	python3 compare.py

clean:
	rm -rf build

.PHONY: check clean
