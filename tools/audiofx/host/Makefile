# Host builds of the firmware's audio DSP and visualizer styles 7-14, held to the Python references.
#   make -C tools/audiofx/host check      the DSP against dsp.py on every test WAV
#   make -C tools/audiofx/host wow        the eight effects against effects_wow.py, pixel for pixel
SRC      := ../../../src/audio
VIZ      := ../../../src/viz/wow
CXX      ?= c++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -I$(SRC)
# No fused multiply-add: Python cannot contract a*b+c, so the C++ must not either.
WOWFLAGS := $(CXXFLAGS) -ffp-contract=off -I$(VIZ)
WOW_SRC  := $(VIZ)/viz_frame.cpp $(VIZ)/wow_canvas.cpp $(VIZ)/wow_engine.cpp $(VIZ)/wow_bars.cpp $(VIZ)/wow_glow.cpp $(VIZ)/wow_matrix.cpp
WOW_DEPS := test_wow.cpp wav_io.h $(WOW_SRC) $(wildcard $(VIZ)/*.h) $(SRC)/audio_dsp.cpp $(SRC)/audio_dsp.h

build/test_dsp: test_dsp.cpp $(SRC)/audio_dsp.cpp $(SRC)/audio_dsp.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test_dsp.cpp $(SRC)/audio_dsp.cpp

build/test_wow_d: $(WOW_DEPS)
	mkdir -p build
	$(CXX) $(WOWFLAGS) -DWOW_REAL=double -o $@ test_wow.cpp $(WOW_SRC) $(SRC)/audio_dsp.cpp

build/test_wow_f: $(WOW_DEPS)
	mkdir -p build
	$(CXX) $(WOWFLAGS) -DWOW_REAL=float -o $@ test_wow.cpp $(WOW_SRC) $(SRC)/audio_dsp.cpp

check: build/test_dsp
	python3 compare.py

wow: build/test_wow_d build/test_wow_f
	python3 compare_wow.py

clean:
	rm -rf build

.PHONY: check wow clean
