# Standalone build of the NChilada reader (src/NChiladaReader.*): a PLAIN C++
# compiler, NO Charm headers on the include path, NO Charm libraries at link,
# and no data files -- the test writes its own fixtures. Same shape as
# tests/treecache.
BASE_PATH := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CXX ?= c++
CXXFLAGS = -std=gnu++17 -O2 \
           -I$(BASE_PATH)/src -I$(BASE_PATH)/utility/structures

# The reader parses field headers with xdr_template, so XDR is needed at
# link. Modern glibc moved it to libtirpc; older ones have it in libc.
TIRPC_INC := $(shell test -d /usr/include/tirpc && echo -I/usr/include/tirpc)
TIRPC_LIB := $(shell echo 'int main(){return 0;}' | $(CC) -xc - -ltirpc -o /dev/null >/dev/null 2>&1 && echo -ltirpc)

all: nchilada-test

nchilada-test: test.cpp $(BASE_PATH)/src/NChiladaReader.C $(BASE_PATH)/src/NChiladaReader.h
	$(CXX) $(CXXFLAGS) $(TIRPC_INC) -o nchilada-test test.cpp \
	    $(BASE_PATH)/src/NChiladaReader.C $(TIRPC_LIB)

test: all
	rm -rf nchilada-fixture
	./nchilada-test ./nchilada-fixture

clean:
	rm -f nchilada-test
	rm -rf nchilada-fixture
