// Module translation unit for the fof module: compiles the non-template part
// of fof.def.h (module registration, called from the application mainmodule's
// registration chain via its `extern module fof`) into libfof.a. Everything
// else in this module is templated headers instantiated by the application
// (see fof-templates.h and examples/fof3).
#include "Paratreet.h"
#include "FoFPhase3.h"

// Template definitions of both modules arrive via the headers above
// (templates.h through Subtree.h; fof-templates.h through FoFPhase1.h).
// The plain include below compiles the non-template module-registration
// part of fof.def.h. FragData instantiations triggered here duplicate the
// app TU's; the linker deduplicates them.
#include "fof.def.h"
