/* Host shim: main.c calls QNX setprio() under PLATFORM_QNX. */
#ifndef PARITY_SHIM_NEUTRINO_H
#define PARITY_SHIM_NEUTRINO_H
static inline int setprio(int pid, int prio) { (void)pid; (void)prio; return 0; }
#endif
