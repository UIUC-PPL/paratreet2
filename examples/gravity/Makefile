include ../../src/Makefile.common

LBS = CommonLBs PrefixLB OrbLB
LB_LIBS = $(foreach m, $(LBS), -module $(m))
OPTS = -g -O3 $(INCLUDES) -DCOUNT_INTERACTIONS=0 -DDEBUG=0 $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: Gravity
DATA = GravityData.h
VISITORS = GravityVisitor.h

Main.decl.h: Main.ci $(DATA)
	$(CHARMC) $<

Gravity: Main.decl.h Main.o $(PARATREET_PATH)/libparatreet.a
	$(CHARMC) -language charm++ -module CommonLBs -o Gravity Main.o $(LD_LIBS)

Main.o: Main.C Main.h $(VISITORS) $(DATA) Main.decl.h
	$(CHARMC) -c $<

# theta 0.7 accuracy band + theta->0 exactness (walk degenerates to the
# same softened direct sum), single- and multi-process. Every run must
# print GRAVITY TEST PASSED.
test: all
	./Gravity -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p2 ++ppn 2
	./Gravity -f $(BASE_PATH)/inputs/1k.tipsy -d oct -o 0 +p2 ++ppn 2
	./charmrun ++local ./Gravity -f $(BASE_PATH)/inputs/1k.tipsy -d oct +p4 ++ppn 2
	./charmrun ++local ./Gravity -f $(BASE_PATH)/inputs/1k.tipsy -d oct -o 0 +p4 ++ppn 2
	./Gravity -f $(BASE_PATH)/inputs/10k.tipsy -d oct +p2 ++ppn 2
	./charmrun ++local ./Gravity -f $(BASE_PATH)/inputs/10k.tipsy -d oct -o 0 +p4 ++ppn 2
	./charmrun ++local ./Gravity -f $(BASE_PATH)/inputs/1k.tipsy -d oct -i 5 +p4 ++ppn 2

clean:
	rm -f *.decl.h *.def.h conv-host *.o Gravity charmrun
