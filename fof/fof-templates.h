// Template instantiation header for the fof module, included once in the
// application's mainmodule TU alongside the core's templates.h (see
// examples/fof3/Main.C). Pulls in the templated part of fof.def.h so the
// app's Data instantiations of the FoF chares are compiled there.
#ifndef FOF_TEMPLATES_H_
#define FOF_TEMPLATES_H_

#define CK_TEMPLATES_ONLY
#include "fof.def.h"
#undef CK_TEMPLATES_ONLY

#endif // FOF_TEMPLATES_H_
