include ../../src/Makefile.common

LBS = CommonLBs PrefixLB OrbLB
LB_LIBS = $(foreach m, $(LBS), -module $(m))
OPTS = -g -O3 $(INCLUDES) -DCOUNT_INTERACTIONS=0 -DDEBUG=0 $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: Annotate
DATA = MinMaxData.h
VISITORS = CheckVisitor.h

Main.decl.h: Main.ci $(DATA)
	$(CHARMC) $<

Annotate: Main.decl.h Main.o Annotate.o $(PARATREET_PATH)/libparatreet.a
	$(CHARMC) -language charm++ -module CommonLBs -o Annotate Annotate.o Main.o $(LD_LIBS)

Annotate.o: Annotate.C Main.h $(VISITORS) $(DATA) Main.decl.h
	$(CHARMC) -c $<

Main.o: Main.C Main.h $(VISITORS) $(DATA) Main.decl.h
	$(CHARMC) -c $<

test: all
	./Annotate -f $(BASE_PATH)/inputs/1k.tipsy -d sfc +p2
	./Annotate -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p2
	./charmrun ++local ./Annotate -f $(BASE_PATH)/inputs/1k.tipsy -d sfc +p4 ++ppn 2
	./charmrun ++local ./Annotate -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p4 ++ppn 2

clean:
	rm -f *.decl.h *.def.h conv-host *.o Annotate charmrun
