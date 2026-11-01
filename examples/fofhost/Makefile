include ../../src/Makefile.common

# Stand-in host for the FoF3 plug-in (design/plugin-interface.md). It links
# the plug-in the way ChaNGa would: `-module fofplugin`, with no paratreet
# headers in its own sources (only fof/FoFPluginAPI.h).
OPTS = -g -O3 $(INCLUDES) $(FOF_INCLUDES) -DDEBUG=0 $(GPU_OPTS) $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: fofhost

fofhost.decl.h: fofhost.ci
	$(CHARMC) $<

Host.o: Host.C fofhost.decl.h $(FOF_PATH)/FoFPluginAPI.h
	$(CHARMC) -c $<

fofhost: Host.o $(FOF_PATH)/libmodulefofplugin.a $(FOF_PATH)/libfof.a $(PARATREET_PATH)/libparatreet.a $(FOF_GPU_LIB)
	$(CHARMC) -language charm++ -module CommonLBs -L$(FOF_PATH) -module fofplugin -o fofhost Host.o $(FOF_LD_LIBS) $(FOF_GPU_LD_LIBS) $(LD_LIBS)

clean:
	rm -f *.decl.h *.def.h conv-host *.o fofhost charmrun
