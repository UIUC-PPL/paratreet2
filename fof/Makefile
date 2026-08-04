include ../src/Makefile.common

# The fof module: FoF chares + drivers over the app-agnostic core (see
# fof.ci). Build order: sibling unionfind stack first (see the top-level
# README), then src/ (paratreet.decl.h), then this, then the FoF examples.
OPTS = -g -O3 $(INCLUDES) $(FOF_INCLUDES) -DDEBUG=0 $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)

all: libfof.a

# unionFindLib.decl.h comes from the pre-built sibling (on the include path
# via FOF_INCLUDES); fof.ci extern-references its module and paratreet's.
fof.decl.h: fof.ci $(UNION_FIND_DIR)/unionFindLib.decl.h
	$(CHARMC) $<

FoF.o: FoF.C FoFData.h FoFPhase1.h FoFPhase3.h fof.decl.h $(PARATREET_PATH)/paratreet.decl.h
	$(CHARMC) -c $<

libfof.a: FoF.o
	ar cr libfof.a FoF.o
	ranlib libfof.a

clean:
	rm -f *.decl*.h *.def*.h conv-host *.o *.a
