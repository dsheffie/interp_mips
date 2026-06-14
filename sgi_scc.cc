#include "sgi_scc.hh"
#include <cstdio>

/* RR0 status bits: bit2 = Tx Buffer Empty, bit6 = Tx Underrun/EOM ("all sent").
 * Reporting both (and Rx-char-available = bit0 = 0) makes the IRIX du driver's
 * "ready to transmit?" poll always succeed. */
static const uint8_t RR0_TX_EMPTY = 0x04;
static const uint8_t RR0_ALL_SENT = 0x40;

/* register index from the byte offset: 0 ctrlB, 1 dataB, 2 ctrlA, 3 dataA.
 * data registers are the odd indices (ab_dc bit0 = data). */
static inline bool is_data_reg(uint32_t offs) { return ((offs >> 2) & 1u) != 0u; }

uint8_t sgi_scc::read(uint32_t offs) {
  if(!is_data_reg(offs)) {
    return RR0_TX_EMPTY | RR0_ALL_SENT;   /* control read -> RR0 */
  }
  return 0;                                /* data read -> no Rx char */
}

void sgi_scc::write(uint32_t offs, uint8_t b) {
  if(is_data_reg(offs)) {
    putchar((int)b);                       /* transmit the console byte */
    fflush(stdout);
  }
  /* control / WR-pointer writes ignored: TX-only model */
}
