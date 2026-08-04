# Standalone build of the passive TreeCache core: a PLAIN C++ compiler,
# NO Charm headers on the include path, NO Charm libraries at link
# (design/smp-cache-extraction.md phase 3 gate). Particle.C is compiled
# in for the Particle member definitions the templates reference.
BASE_PATH := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CXX ?= c++
CXXFLAGS = -std=gnu++17 -O2 -pthread \
           -I$(BASE_PATH)/src -I$(BASE_PATH)/utility/structures

all: treecache-test

treecache-test: test.cpp $(BASE_PATH)/src/TreeCache.h $(BASE_PATH)/src/Node.h
	$(CXX) $(CXXFLAGS) -o treecache-test test.cpp $(BASE_PATH)/src/Particle.C \
	    $(BASE_PATH)/utility/structures/hilbert.c

test: all
	./treecache-test

clean:
	rm -f treecache-test
