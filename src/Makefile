include Makefile.common

# add -DFOF -DUNIONFIND to build the Friends-of-Friends support (requires the
# unionfind and htram sibling libraries; see Makefile.common)
OPTS = -g -Ofast $(INCLUDES) -DDEBUG=0 $(MAKE_OPTS) -DFOF -DUNIONFIND 
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

OBJS = Paratreet.o Loadable.o Reader.o Writer.o Particle.o BoundingBox.o Decomposition.o Modularization.o TreeSpec.o ThreadStateHolder.o
# Distributed union-find (FoF step 4) is the sibling UIUC-PPL/unionfind
# library (branch fof_with_aggregation), linked via Makefile.common. Build the
# sibling FIRST, AGGREGATION-off (plain sends; htram linked but dormant):
#   cd ../../unionfind && make AGGREGATION= PROFILE=
TIPSY_OBJS = NChilReader.o SS.o TipsyFile.o TipsyReader.o hilbert.o

UTILITY_HEADERS = common.h Utility.h $(STRUCTURE_PATH)/Vector3D.h $(STRUCTURE_PATH)/SFC.h
CORE_HEADERS = BoundingBox.h BufferedVec.h MultiData.h Node.h NodeWrapper.h ParticleMsg.h Splitter.h
IMPL_HEADERS = CacheManager.h Configuration.h Driver.h Partition.h Reader.h Resumer.h Splitter.h Subtree.h Traverser.h TreeCanopy.h

LBS = PrefixLB OrbLB #AverageSmoothLB DiffusionLB DistributedPrefixLB DistributedOrbLB
LB_LIBS = $(foreach m, $(LBS), libmodule$(m).a)

all: lib

lib: $(OBJS) $(LB_LIBS)
	ar x $(STRUCTURE_PATH)/libTipsy.a
	ar cr libparatreet.a $(OBJS) $(TIPSY_OBJS)
	rm -f $(TIPSY_OBJS)
	ranlib libparatreet.a

# unionFindLib.decl.h comes from the pre-built sibling (on the include path
# via -I$(UNION_FIND_DIR)); paratreet.ci extern-references its module.
paratreet.decl.h: paratreet.ci $(UNION_FIND_DIR)/unionFindLib.decl.h
	$(CHARMC) $<

Paratreet.o: $(UTILITY_HEADERS) $(CORE_HEADERS) $(IMPL_HEADERS) paratreet.decl.h
	$(CHARMC) -c Paratreet.C

%.o: %.C %.h paratreet.decl.h
	$(CHARMC) -c $<

libmodule%.a : loadbalancers/%.ci loadbalancers/%.C loadbalancers/%.h LBCommon.h
	$(CHARMC) $<
	$(CHARMC) -o $@ $(word 2,$^)

clean:
	rm -f *.decl*.h *.def*.h conv-host *.o *.a
