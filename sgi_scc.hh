#ifndef __sgi_scc__
#define __sgi_scc__

#include <cstdint>
#include <cstddef>

struct state_t;

/* Minimal Zilog Z8530 / SCC85230 (SGI IP22 IOC2 serial) -- TX-only, just enough
 * to capture the IRIX serial console to stdout.  The IRIX kernel drives this
 * chip directly (it does NOT use the ARCS romvec Write), so this is what makes
 * boot text appear.
 *
 * 4 byte-wide registers at 4-byte spacing, in z80scc "ab_dc" order
 * (offset bit1 = channel A/B, bit0 = data/control):
 *   +0x0  ctrl B   +0x4  data B   +0x8  ctrl A   +0xc  data A
 * A write to either DATA register transmits the byte (-> stdout); a read of a
 * CONTROL register returns RR0 with Tx-Buffer-Empty set so the du driver always
 * believes it can transmit.  Control/pointer writes are ignored.
 *
 * Base address (0x1fbd9830, IOC2 reg 0x0c) is the canonical IP22 value; confirm
 * against the MAME scc_dc_w guest-physical capture when wiring a real IRIX boot. */
class sgi_scc {
  state_t *s;
public:
  sgi_scc(state_t *s) : s(s) {}
  uint8_t read(uint32_t offs);          /* offs within the 16-byte SCC window */
  void    write(uint32_t offs, uint8_t b);
};

#endif
