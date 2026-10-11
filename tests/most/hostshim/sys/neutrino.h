/* Host shim for QNX <sys/neutrino.h> as screen.h uses it: declaration macros and the
 * fixed-width type names.  Test-only; the ARM build uses the real SDP headers. */
#ifndef HOSTSHIM_SYS_NEUTRINO_H
#define HOSTSHIM_SYS_NEUTRINO_H
#include <sys/cdefs.h>
#include <signal.h>
#include <stdint.h>
typedef int32_t _Int32t;
typedef uint32_t _Uint32t;
#endif
