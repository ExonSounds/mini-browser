CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
PKGS     := gtk4 webkitgtk-6.0 libsoup-3.0 gstreamer-1.0 libdrm

CXXFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

SOURCES  := main.cpp vblank.cpp

browser: $(SOURCES) vblank.h
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@ $(LDLIBS)

run: browser
	./browser

clean:
	rm -f browser

.PHONY: run clean
