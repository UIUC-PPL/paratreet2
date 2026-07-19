# Input generators for paratreet2 (copied from the old paratreet checkout's
# inputgen/, which is kept read-only; see its README for the workflow):
#   ./plummer 0 <nbody> out.dat        # Plummer model -> raw .dat
#   ./uniform <seed> <nbody> out.dat   # uniform unit box -> same .dat format
#   ./tipsyPlummer out.dat out.tipsy   # .dat -> tipsy (works for either)
# STRUCTURES_PATH points at this tree's utility/structures (Vector3D.h,
# OrientedBox.h, TipsyFile.h, libTipsy.a).
STRUCTURES_PATH = ../utility/structures
TARGET = plummer tipsyPlummer uniform

all: $(TARGET)

OBJECTS = plummer.o tipsyPlummer.o gen_util.o uniform.o

# CXX is make's built-in default (g++); override with `make CXX=...`.
CPPFLAGS += -I$(STRUCTURES_PATH) -O2 -std=c++11

$(STRUCTURES_PATH)/libTipsy.a: $(STRUCTURES_PATH)/Makefile
	cd $(STRUCTURES_PATH); $(MAKE) libTipsy.a

$(STRUCTURES_PATH)/Makefile:
	cd $(STRUCTURES_PATH); ./configure

plummer.o: plummer.cpp
	$(CXX) $(CPPFLAGS) -c plummer.cpp

tipsyPlummer.o: tipsyPlummer.cpp
	$(CXX) $(CPPFLAGS) -c tipsyPlummer.cpp

gen_util.o: gen_util.cpp
	$(CXX) $(CPPFLAGS) -c gen_util.cpp

uniform.o: uniform.cpp
	$(CXX) $(CPPFLAGS) -c uniform.cpp

plummer: plummer.o gen_util.o
	$(CXX) $(CPPFLAGS) -o plummer plummer.o gen_util.o

uniform: uniform.o
	$(CXX) $(CPPFLAGS) -o uniform uniform.o

tipsyPlummer: tipsyPlummer.o $(STRUCTURES_PATH)/libTipsy.a
	$(CXX) $(CPPFLAGS) -o tipsyPlummer tipsyPlummer.o -lTipsy -L$(STRUCTURES_PATH)

clean:
	rm -rf $(OBJECTS) $(TARGET)

.PHONY: all clean
