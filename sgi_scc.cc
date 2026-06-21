#include "sgi_scc.hh"
#include <cstdio>

/* RR0: bit2 Tx Buffer Empty (ready for a char), bit6 All Sent (shift done). */
static const uint8_t RR0_TX_EMPTY = 0x04;
static const uint8_t RR0_ALL_SENT = 0x40;

/* WR1: Tx Int Enable. (zs.h) */
static const uint8_t TxINT_ENAB = 0x02;

/* RR3 chip-wide interrupt-pending bits (zs.h): channel A/B Tx-IP. */
static const uint8_t CHBTxIP = 0x02;
static const uint8_t CHATxIP = 0x10;

/* WR0 command field (bits[5:3]). */
static const uint8_t WR0_CMD_MASK   = 0x38;
static const uint8_t CMD_POINT_HIGH = 0x08;  /* add 8 to the register pointer */
static const uint8_t CMD_RES_Tx_P   = 0x28;  /* Reset Tx Int Pending */

/* Instruction ticks for a char to shift out.  Must be long enough that IP2 is
 * genuinely LOW between completions (so the kernel erets to userspace and makes
 * forward progress instead of storming); short enough not to throttle boot. */
static const uint64_t TX_DRAIN_TICKS = 1024;

/* offset bit2 = data/control (ab_dc bit0); offset bit3 = channel (0=B, 1=A). */
static inline bool is_data_reg(uint32_t offs) { return ((offs >> 2) & 1u) != 0u; }
static inline int  chan(uint32_t offs)        { return (int)((offs >> 3) & 1u); }

/* per-channel Tx interrupt pending, gated live on WR1.TxINT_ENAB (matches the
 * Z8530 get_ip(): clearing TxIE deasserts immediately). */
static inline bool tx_int(uint8_t wr1, bool ip) { return (wr1 & TxINT_ENAB) && ip; }

/* advance transmit timing: a shifting char completes -> raise the Tx-IP edge.
 * Called once per executed instruction. */
void sgi_scc::tick() {
  clk++;
  for(int ch = 0; ch < 2; ch++) {
    if(tx_busy[ch] && clk >= drain_at[ch]) {
      tx_busy[ch] = false;                   /* shift complete: buffer empty */
      tx_ip[ch]   = true;                     /* Tx-buffer-empty interrupt (edge) */
    }
  }
}

uint8_t sgi_scc::read(uint32_t offs) {
  int ch = chan(offs);
  if(is_data_reg(offs)) {
    return 0;                                /* data read -> no Rx char (TX-only) */
  }
  /* control read -> RR[pointer]; pointer auto-resets to 0 after the access */
  uint8_t p = ptr[ch];
  ptr[ch] = 0;
  if(p == 3) {                               /* RR3 = chip-wide interrupt-pending */
    uint8_t r3 = 0;
    if(tx_int(wr1[1], tx_ip[1])) r3 |= CHATxIP;
    if(tx_int(wr1[0], tx_ip[0])) r3 |= CHBTxIP;
    return r3;
  }
  /* RR0: Tx Buffer Empty + All Sent iff no char is currently shifting out */
  return (uint8_t)(tx_busy[ch] ? 0 : (RR0_TX_EMPTY | RR0_ALL_SENT));
}

void sgi_scc::write(uint32_t offs, uint8_t b) {
  int ch = chan(offs);
  if(is_data_reg(offs)) {
    putchar((int)b);                         /* echo the byte (order preserved) */
    fflush(stdout);
    /* Writing the next char re-arms: clears the pending Tx int (deasserts IP2),
     * and the char now shifts out (completes TX_DRAIN_TICKS later -> Tx-IP). */
    tx_ip[ch]   = false;
    tx_busy[ch] = true;
    drain_at[ch] = clk + TX_DRAIN_TICKS;
    return;
  }
  /* control write: Z8530 two-step pointer */
  uint8_t p = ptr[ch];
  if(p == 0) {                               /* WR0 */
    uint8_t cmd = b & WR0_CMD_MASK;
    if(cmd == CMD_RES_Tx_P) tx_ip[ch] = false;  /* explicit Tx-int ack */
    ptr[ch] = (uint8_t)((b & 0x07) | ((cmd == CMD_POINT_HIGH) ? 0x08 : 0x00));
  } else {                                   /* write WR[pointer] */
    if(p == 1)      wr1[ch] = b;             /* TxINT_ENAB etc. (per channel) */
    else if(p == 9) wr9     = b;            /* MIE (stored; gating is on TxIE) */
    ptr[ch] = 0;
  }
}

bool sgi_scc::int_pending() {
  return tx_int(wr1[0], tx_ip[0]) || tx_int(wr1[1], tx_ip[1]);
}
