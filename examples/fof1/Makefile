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
test: all
	./FoF1 -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p1
	./FoF1 -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p2
	./FoF1 -f $(BASE_PATH)/inputs/100.tipsy -d oct +p1
	./FoF1 -f $(BASE_PATH)/inputs/100.tipsy -d oct +p2

clean:
	rm -f *.decl.h *.def.h conv-host *.o FoF1 charmrun
