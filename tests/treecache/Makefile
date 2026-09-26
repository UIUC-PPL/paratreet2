# Standalone build of the passive TreeCache core: a PLAIN C++ compiler,
# NO Charm headers on the include path, NO Charm libraries at link
# (design/smp-cache-extraction.md phase 3 gate). Particle.C is compiled
# in for the Particle member definitions the templates reference.
BASE_PATH := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CXX ?= c++
CXXFLAGS = -std=gnu++17 -O2 -pthread \
           -I$(BASE_PATH)/src -I$(BASE_PATH)/utility/structures

all: treecache-test treecache-core-test

treecache-test: test.cpp $(BASE_PATH)/src/TreeCache.h $(BASE_PATH)/src/TreeCacheCore.h $(BASE_PATH)/src/Node.h
	$(CXX) $(CXXFLAGS) -o treecache-test test.cpp $(BASE_PATH)/src/Particle.C \
	    $(BASE_PATH)/utility/structures/hilbert.c

# treecache-core-test: TreeCacheCore over a node type that is NOT
# paratreet2's Node<Data>, with ONLY TreeCacheCore.h on the include chain
# (no Node.h / Particle.h / common.h) — the Traits-contract gate.
treecache-core-test: core_test.cpp $(BASE_PATH)/src/TreeCacheCore.h
	$(CXX) -std=gnu++17 -O2 -pthread -I$(BASE_PATH)/src -o treecache-core-test core_test.cpp

test: all
	./treecache-test
	./treecache-core-test

clean:
	rm -f treecache-test treecache-core-test
