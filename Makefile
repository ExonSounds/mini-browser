CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
PKGS     := gtk4 webkitgtk-6.0 libsoup-3.0 gstreamer-1.0

CXXFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

browser: main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

run: browser
	./browser

clean:
	rm -f browser

.PHONY: run clean
