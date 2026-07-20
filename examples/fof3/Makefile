include ../../src/Makefile.common

LBS = CommonLBs PrefixLB OrbLB
LB_LIBS = $(foreach m, $(LBS), -module $(m))
OPTS = -g -O3 $(INCLUDES) -DCOUNT_INTERACTIONS=0 -DDEBUG=0 $(MAKE_OPTS) -DFOF -DUNIONFIND
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: FoF3
# FragData/FoFPhase3 live in the library; track them explicitly since
# Makefile.common only adds -I flags, not header dependencies.
DATA = $(PARATREET_PATH)/FoFData.h $(PARATREET_PATH)/FoFPhase3.h
VISITORS = FragCheckVisitor.h

Main.decl.h: Main.ci $(DATA)
	$(CHARMC) $<

FoF3: Main.decl.h Main.o FoF3.o $(PARATREET_PATH)/libparatreet.a
	$(CHARMC) -language charm++ -module CommonLBs -o FoF3 FoF3.o Main.o $(LD_LIBS)

FoF3.o: FoF3.C Main.h $(VISITORS) $(DATA) Main.decl.h
	$(CHARMC) -c $<

Main.o: Main.C Main.h $(VISITORS) $(DATA) Main.decl.h
	$(CHARMC) -c $<

# Single-process runs (+p1, +p2): phase 3 finds few or no cross-process
# edges and the run must still pass. Two-process runs (charmrun ++local,
# netlrts SMP over TCP loopback): +p2 ++ppn 1 = 2 procs x 1 PE; +p4 ++ppn 2
# = 2 procs x 2 PEs, exercising phase-1 phaseB and phase 3 together.
# Full 3a matrix (design/step3.md §6): {100, 1k, 10k} x the four configs.
# The serial O(n^2) end-to-end comparison runs everywhere (10k = 5e7 pairs).
test: all
	./FoF3 -f $(BASE_PATH)/inputs/100.tipsy -d oct +p1
	./FoF3 -f $(BASE_PATH)/inputs/100.tipsy -d oct +p2
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/100.tipsy -d oct +p2 ++ppn 1
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/100.tipsy -d oct +p4 ++ppn 2
	./FoF3 -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p1
	./FoF3 -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p2
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p2 ++ppn 1
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p4 ++ppn 2
	./FoF3 -f $(BASE_PATH)/inputs/10k.tipsy -d oct +p1
	./FoF3 -f $(BASE_PATH)/inputs/10k.tipsy -d oct +p2
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/10k.tipsy -d oct +p2 ++ppn 1
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/10k.tipsy -d oct +p4 ++ppn 2

clean:
	rm -f *.decl.h *.def.h conv-host *.o FoF3 charmrun
