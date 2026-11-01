include ../src/Makefile.common

# The fof module: FoF chares + drivers over the app-agnostic core (see
# fof.ci). Build order: sibling unionfind stack first (see the top-level
# README), then src/ (paratreet.decl.h), then this, then the FoF examples.
# GPU=1 compiles the device staging chain into FoFPhase1.h
# (design/phase1-gpu.md stage 0). The device library itself is built
# separately by hipcc in fof/gpu; this only adds the POD header and the
# HIP platform macro hapi.h needs (trap 4).
OPTS = -g -O3 $(INCLUDES) $(FOF_INCLUDES) -DDEBUG=0 $(GPU_OPTS) $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: libfof.a libmodulefofplugin.a

# unionFindLib.decl.h comes from the pre-built sibling (on the include path
# via FOF_INCLUDES); fof.ci extern-references its module and paratreet's.
fof.decl.h: fof.ci $(UNION_FIND_DIR)/unionFindLib.decl.h
	$(CHARMC) $<

FoF.o: FoF.C FoFData.h FoFPhase1.h FoFPhase3.h fof.decl.h $(PARATREET_PATH)/paratreet.decl.h
	$(CHARMC) -c $<

libfof.a: FoF.o
	ar cr libfof.a FoF.o
	ranlib libfof.a

# The in-situ plug-in (design/plugin-interface.md): hosted app main + the
# FoFPluginAPI.h entry points, for host programs only (FoF3 has its own main).
# Named libmodule*.a so a host links it with `charmc -module fofplugin`,
# which registers the module without an `extern module` in the host's .ci
# (that would pull paratreet's generated headers, and their TreePiece, into
# the host's own).
fofplugin.decl.h: FoFPlugin.ci fof.decl.h
	$(CHARMC) $<

FoFPlugin.o: FoFPlugin.C FoFPluginAPI.h FoFData.h FoFPhase1.h FoFPhase3.h fofplugin.decl.h fof.decl.h $(PARATREET_PATH)/paratreet.decl.h
	$(CHARMC) -c $<

libmodulefofplugin.a: FoFPlugin.o
	ar cr libmodulefofplugin.a FoFPlugin.o
	ranlib libmodulefofplugin.a

clean:
	rm -f *.decl*.h *.def*.h conv-host *.o *.a
