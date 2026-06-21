#include "sgi_scc.hh"
#include <cstdio>
#include <cstdlib>
static const bool scc_dbg = getenv("SCC_DBG") != nullptr;

/* RR0 status bits: bit2 = Tx Buffer Empty, bit6 = Tx Underrun/EOM ("all sent").
 * Reporting both (and Rx-char-available = bit0 = 0) makes a "ready to transmit?"
 * poll always succeed. */
static const uint8_t RR0_TX_EMPTY = 0x04;
static const uint8_t RR0_ALL_SENT = 0x40;

/* WR1: Tx Int Enable.  WR9: Master Interrupt Enable. (zs.h) */
static const uint8_t TxINT_ENAB = 0x02;
static const uint8_t MIE        = 0x08;

/* RR3 chip-wide interrupt-pending bits (zs.h): channel A/B Tx-IP. */
static const uint8_t CHBTxIP = 0x02;
static const uint8_t CHATxIP = 0x10;

/* WR0 command field (bits[5:3]) values we act on. */
static const uint8_t WR0_CMD_MASK = 0x38;
static const uint8_t CMD_POINT_HIGH = 0x08;  /* add 8 to the register pointer */
static const uint8_t CMD_RES_Tx_P   = 0x28;  /* Reset Tx Int Pending */

/* offset bit2 = data/control (ab_dc bit0); offset bit3 = channel (0=B, 1=A). */
static inline bool is_data_reg(uint32_t offs) { return ((offs >> 2) & 1u) != 0u; }
static inline int  chan(uint32_t offs)        { return (int)((offs >> 3) & 1u); }

uint8_t sgi_scc::read(uint32_t offs) {
  int ch = chan(offs);
  if(is_data_reg(offs)) {
    return 0;                                 /* data read -> no Rx char (TX-only) */
  }
  /* control read -> RR[pointer]; pointer auto-resets to 0 after the access */
  uint8_t p = ptr[ch];
  ptr[ch] = 0;
  uint8_t rv;
  if(p == 3) {                                /* RR3 = chip-wide interrupt-pending */
    rv = 0;
    if(tx_ip[1]) rv |= CHATxIP;
    if(tx_ip[0]) rv |= CHBTxIP;
  } else {
    rv = RR0_TX_EMPTY | RR0_ALL_SENT;         /* RR0 */
  }
  /* log only control reads that LACK Tx_BUF_EMP (these stall put_char's poll) */
  if(scc_dbg && !(rv & RR0_TX_EMPTY)) { static int n=0; if(n++ < 30) fprintf(stderr,"[scc] rd-noTXE ch%d offs=%x ptr=%d -> %02x (tx_ip=%d%d wr9=%02x)\n", ch, offs, p, rv, tx_ip[0],tx_ip[1], wr9); }
  return rv;
}

void sgi_scc::write(uint32_t offs, uint8_t b) {
  int ch = chan(offs);
  if(is_data_reg(offs)) {
    putchar((int)b);                          /* transmit the console byte */
    fflush(stdout);
    /* instant TX: the buffer is now empty -> arm the Tx-buffer-empty interrupt */
    if((wr1[ch] & TxINT_ENAB) && (wr9 & MIE)) tx_ip[ch] = true;
    if(scc_dbg) {
      static int n = 0;
      if(n++ < 8) fprintf(stderr, "[scc] data ch%d '%c' tx_ip=%d (wr1=%02x wr9=%02x)\n",
                          ch, (b>=32&&b<127)?b:'.', tx_ip[ch], wr1[ch], wr9);
    }
    return;
  }
  /* control write: Z8530 two-step pointer */
  uint8_t p = ptr[ch];
  if(p == 0) {                                /* WR0 */
    uint8_t cmd = b & WR0_CMD_MASK;
    if(cmd == CMD_RES_Tx_P) tx_ip[ch] = false;/* ack Tx int (kernel's drain-done) */
    ptr[ch] = (uint8_t)((b & 0x07) | ((cmd == CMD_POINT_HIGH) ? 0x08 : 0x00));
  } else {                                    /* write WR[pointer] */
    if(p == 1)      wr1[ch] = b;              /* TxINT_ENAB etc. (per channel) */
    else if(p == 9) wr9     = b;             /* MIE (chip-wide; either channel) */
    if(scc_dbg && (p==1 || p==9))
      fprintf(stderr, "[scc] ch%d WR%d=%02x (wr1A=%02x wr1B=%02x wr9=%02x)\n",
              ch, p, b, wr1[1], wr1[0], wr9);
    ptr[ch] = 0;
  }
}

bool sgi_scc::int_pending() {
  return (wr9 & MIE) && (tx_ip[0] || tx_ip[1]);
}
