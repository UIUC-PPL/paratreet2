# treecache: the node-type-independent core of the SMP tree cache
# (TreeCacheCore.h), header-only, no Charm dependency. This Makefile builds
# and runs its standalone unit test with a PLAIN C++ compiler: no Charm
# headers, no Charm libraries, nothing else from paratreet2 on the include
# path. That is the gate for "a host application can take this directory
# alone".
CXX ?= c++
CXXFLAGS = -std=gnu++17 -O2 -pthread -I.

all: treecache-core-test

treecache-core-test: core_test.cpp TreeCacheCore.h
	$(CXX) $(CXXFLAGS) -o treecache-core-test core_test.cpp

test: all
	./treecache-core-test

clean:
	rm -f treecache-core-test
