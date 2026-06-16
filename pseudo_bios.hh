#ifndef __pseudo_bios_hh__
#define __pseudo_bios_hh__

struct state_t;
class sparse_mem;

/*
 * Pseudo-BIOS: synthesize the SGI ARCS pre-kernel platform state the IRIX
 * kernel consumes, directly in RAM, in place of a real PROM+sash boot.
 *
 * Stage 1 (here): the argc/argv/envp handoff that `sash` gives `/unix` --
 * a0=argc, a1=argv, a2=envp, with the argv/envp arrays + strings laid out in
 * RAM as kseg0 pointers (the kernel's `getargs`/`_envirn` read these directly).
 * Ground-truth strings/values measured from MAME (MAME_QUESTIONS.md Q5).
 *
 * Future stages will grow this to also build the SPB / romvec / PrivateVector /
 * env block, driven by the MAME co-sim as the kernel touches them.
 */
void install_pseudo_bios(state_t *s, sparse_mem *sm);

#endif
