include ../../src/Makefile.common

LBS = CommonLBs PrefixLB OrbLB #AverageSmoothLB DiffusionLB DistributedPrefixLB DistributedOrbLB
LB_LIBS = $(foreach m, $(LBS), -module $(m))
OPTS = -g -O3 $(INCLUDES) -DCOUNT_INTERACTIONS=0 -DDEBUG=0 $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: VisitAll
DATA = SearchData.h
VISITORS = VisitAllVisitor.h
OTHERS = CountManager.h

debug:
	echo $(LB_LIBS)

Main.decl.h: Main.ci $(DATA)
	$(CHARMC) $<

VisitAll: Main.decl.h Main.o VisitAll.o $(PARATREET_PATH)/libparatreet.a
	$(CHARMC) -language charm++ -module CommonLBs -o VisitAll VisitAll.o Main.o $(LD_LIBS)

VisitAll.o: VisitAll.C Main.h Main.decl.h
	$(CHARMC) -c $<

Main.o: Main.C Main.h $(VISITORS) $(OTHERS) Main.decl.h
	$(CHARMC) -c $<

test: all
	./VisitAll -f $(BASE_PATH)/inputs/1k.tipsy -d sfc +p2
	./charmrun ++local ./VisitAll -f $(BASE_PATH)/inputs/1k.tipsy -d sfc +p4 ++ppn 2

clean:
	rm -f *.decl.h *.def.h conv-host *.o VisitAll charmrun
