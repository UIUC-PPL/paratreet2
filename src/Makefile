include Makefile.common

# add -DFOF -DUNIONFIND to build the Friends-of-Friends support (requires the
# unionfind and htram sibling libraries; see Makefile.common)
OPTS = -g -Ofast $(INCLUDES) -DDEBUG=0 $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

OBJS = Paratreet.o Loadable.o Reader.o Writer.o Particle.o BoundingBox.o Decomposition.o Modularization.o TreeSpec.o ThreadStateHolder.o
# Distributed union-find (FoF step 4), imported from UIUC-PPL/unionfind into
# src/uf2 (see uf2/unionFindLib.h for provenance). Compiled into libparatreet.a.
UF2_OBJS = uf2/unionFindLib.o uf2/prefixBalance.o
TIPSY_OBJS = NChilReader.o SS.o TipsyFile.o TipsyReader.o hilbert.o

UTILITY_HEADERS = common.h Utility.h $(STRUCTURE_PATH)/Vector3D.h $(STRUCTURE_PATH)/SFC.h
CORE_HEADERS = BoundingBox.h BufferedVec.h MultiData.h Node.h NodeWrapper.h ParticleMsg.h Splitter.h
IMPL_HEADERS = CacheManager.h Configuration.h Driver.h Partition.h Reader.h Resumer.h Splitter.h Subtree.h Traverser.h TreeCanopy.h

LBS = PrefixLB OrbLB #AverageSmoothLB DiffusionLB DistributedPrefixLB DistributedOrbLB
LB_LIBS = $(foreach m, $(LBS), libmodule$(m).a)

all: lib

lib: $(OBJS) $(UF2_OBJS) $(LB_LIBS)
	ar x $(STRUCTURE_PATH)/libTipsy.a
	ar cr libparatreet.a $(OBJS) $(UF2_OBJS) $(TIPSY_OBJS)
	rm -f $(TIPSY_OBJS)
	ranlib libparatreet.a

paratreet.decl.h: paratreet.ci uf2/unionFindLib.decl.h
	$(CHARMC) $<

uf2/prefixBalance.decl.h uf2/prefixBalance.def.h: uf2/prefixBalance.ci
	cd uf2 && $(CHARMC) prefixBalance.ci

uf2/unionFindLib.decl.h uf2/unionFindLib.def.h: uf2/unionFindLib.ci uf2/prefixBalance.decl.h
	cd uf2 && $(CHARMC) unionFindLib.ci

uf2/prefixBalance.o: uf2/prefixBalance.C uf2/prefixBalance.h uf2/prefixBalance.decl.h uf2/prefixBalance.def.h
	cd uf2 && $(CHARMC) -c prefixBalance.C

uf2/unionFindLib.o: uf2/unionFindLib.C uf2/unionFindLib.h uf2/types.h uf2/unionFindLib.decl.h uf2/unionFindLib.def.h uf2/prefixBalance.decl.h
	cd uf2 && $(CHARMC) -c unionFindLib.C

Paratreet.o: $(UTILITY_HEADERS) $(CORE_HEADERS) $(IMPL_HEADERS) paratreet.decl.h
	$(CHARMC) -c Paratreet.C

%.o: %.C %.h paratreet.decl.h
	$(CHARMC) -c $<

libmodule%.a : loadbalancers/%.ci loadbalancers/%.C loadbalancers/%.h LBCommon.h
	$(CHARMC) $<
	$(CHARMC) -o $@ $(word 2,$^)

clean:
	rm -f *.decl*.h *.def*.h conv-host *.o *.a
	rm -f uf2/*.decl.h uf2/*.def.h uf2/*.o uf2/charmrun
