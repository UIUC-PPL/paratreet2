include ../../src/Makefile.common

LBS = CommonLBs PrefixLB OrbLB
LB_LIBS = $(foreach m, $(LBS), -module $(m))
OPTS = -g -O3 $(INCLUDES) -DCOUNT_INTERACTIONS=0 -DDEBUG=0 $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: FoF1
# FragData lives in the library (src/FoFData.h); track it explicitly since
# Makefile.common only adds -I flags, not header dependencies.
DATA = $(PARATREET_PATH)/FoFData.h
VISITORS = FragCheckVisitor.h

Main.decl.h: Main.ci $(DATA)
	$(CHARMC) $<

FoF1: Main.decl.h Main.o FoF1.o $(PARATREET_PATH)/libparatreet.a
	$(CHARMC) -language charm++ -module CommonLBs -o FoF1 FoF1.o Main.o $(LD_LIBS)

FoF1.o: FoF1.C Main.h $(VISITORS) $(DATA) Main.decl.h
	$(CHARMC) -c $<

Main.o: Main.C Main.h $(VISITORS) $(DATA) Main.decl.h
	$(CHARMC) -c $<

# On one process phase 1 alone is the complete FoF, so the serial check is
# exact for both +p1 and +p2 (SMP build: +p2 = two worker PEs, one process;
# +p2 exercises the phaseB/merge/relabel cross-PE path).
# fof1 validates phase 1 (intra-process union-find) against a serial FoF,
# which is only equal to the full FoF on a SINGLE process. +p1/+p2 are both
# one process (N worker PEs). Do NOT add a multi-process (++ppn) config here:
# across processes the phase-1 tips are deliberately partial (cross-process
# merging is phase 3's job) so the serial comparison would fail by design.
# Multi-process FoF correctness is covered by examples/fof3.
test: all
	./FoF1 -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p1
	./FoF1 -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p2
	./FoF1 -f $(BASE_PATH)/inputs/100.tipsy -d oct +p1
	./FoF1 -f $(BASE_PATH)/inputs/100.tipsy -d oct +p2

clean:
	rm -f *.decl.h *.def.h conv-host *.o FoF1 charmrun
