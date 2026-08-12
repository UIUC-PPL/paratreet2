include ../../src/Makefile.common

LBS = CommonLBs PrefixLB OrbLB
LB_LIBS = $(foreach m, $(LBS), -module $(m))
OPTS = -g -O3 $(INCLUDES) $(FOF_INCLUDES) -DCOUNT_INTERACTIONS=0 -DDEBUG=0 $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: FoF3
# FragData/FoFPhase3 live in the fof module; track them explicitly since
# Makefile.common only adds -I flags, not header dependencies.
DATA = $(FOF_PATH)/FoFData.h $(FOF_PATH)/FoFPhase3.h $(FOF_PATH)/FoFPhase1.h $(FOF_PATH)/FoFStealTypes.h
VISITORS = FragCheckVisitor.h

Main.decl.h: Main.ci $(DATA)
	$(CHARMC) $<

FoF3: Main.decl.h Main.o FoF3.o $(PARATREET_PATH)/libparatreet.a $(FOF_PATH)/libfof.a $(TRACE_STAMP)
	$(CHARMC) -language charm++ -module CommonLBs -o FoF3 FoF3.o Main.o $(TRACE_FLAGS) $(FOF_LD_LIBS) $(LD_LIBS)

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
	./FoF3 -f $(BASE_PATH)/inputs/1k.tipsy -d oct -w transposed +p2 ++ppn 2
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/1k.tipsy -d oct -w transposed +p4 ++ppn 2
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/1k.tipsy -d oct -u serial +p4 ++ppn 2
	./charmrun ++local ./FoF3 -f $(BASE_PATH)/inputs/10k.tipsy -d oct -u serial +p4 ++ppn 2

clean:
	rm -f *.decl.h *.def.h conv-host *.o FoF3 charmrun $(TRACE_STAMP)

# Trace logs only (see src/Makefile.common); deliberately not part of clean.
cleanp:
	rm -f $(TRACE_OUTPUTS)
