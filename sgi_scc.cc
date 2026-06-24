#include "sgi_scc.hh"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>

static const bool scc_rx_dbg = getenv("SCCRX") != nullptr;

/* RR0 bit0 = Rx Character Available (set while the chanA Rx FIFO is non-empty). */
static const uint8_t RR0_RX_AVAIL = 0x01;

/* RR0: bit2 Tx Buffer Empty (ready for a char), bit6 All Sent (shift done). */
static const uint8_t RR0_TX_EMPTY = 0x04;
static const uint8_t RR0_ALL_SENT = 0x40;

/* WR1: Tx Int Enable. (zs.h) */
static const uint8_t TxINT_ENAB = 0x02;

/* RR3 chip-wide interrupt-pending bits (Z8530, channel A only): per-channel
 * Rx/Tx/Ext groups -- chanA in bits 5,4,3, chanB in bits 2,1,0. */
static const uint8_t CHBTxIP = 0x02;   /* chanB Tx-IP (bit1) */
static const uint8_t CHATxIP = 0x10;   /* chanA Tx-IP (bit4) */
static const uint8_t CHBRxIP = 0x04;   /* chanB Rx-IP (bit2) */
static const uint8_t CHARxIP = 0x20;   /* chanA Rx-IP (bit5) */

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

/* Rx interrupts enabled when WR1 bits 4:3 (Rx Int mode) are non-zero
 * (Rx-Int-on-First / -All-Parity / -All-Special); 00 = Rx Int Disabled. */
static inline bool rx_int_en(uint8_t wr1) { return (wr1 & 0x18u) != 0u; }

/* advance transmit timing by dticks instruction ticks: a shifting char that
 * finishes draining raises the Tx-IP edge.  Called periodically (every INT_POLL
 * instructions) with dticks = instructions elapsed, so TX timing stays in
 * instruction units regardless of the poll period. */
/* Drain whatever the host has typed on stdin into the 8-deep chanA Rx FIFO.
 * stdin is put into non-blocking mode on first use so a read with nothing
 * pending returns immediately. Polled (rate-limited) from tick(). */
void sgi_scc::rx_poll() {
  if(!rx_init) {
    int fl = fcntl(0, F_GETFL, 0);
    if(fl != -1) fcntl(0, F_SETFL, fl | O_NONBLOCK);
    rx_init = true;
  }
  uint8_t c;
  while(rx_count < (uint8_t)sizeof(rx_fifo) && ::read(0, &c, 1) == 1) {
    rx_fifo[rx_wptr] = c;
    rx_wptr = (uint8_t)((rx_wptr + 1) % sizeof(rx_fifo));
    rx_count++;
    if(scc_rx_dbg) fprintf(stderr, "[scc] rx push 0x%02x '%c' count=%d\n", c, (c>=32&&c<127)?c:'.', rx_count);
  }
}

void sgi_scc::tick(uint64_t dticks) {
  clk += dticks;
  if(clk >= rx_poll_at) { rx_poll(); rx_poll_at = clk + 65536; }  /* ~every 64k ticks */
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
    /* DATA read -> pop the front Rx FIFO byte (console input). Served on either
     * channel since stdin is the one console input source. */
    if(scc_rx_dbg) fprintf(stderr, "[scc] data read ch=%d offs=%x rx_count=%d\n", ch, offs, rx_count);
    if(rx_count > 0) {
      uint8_t c = rx_fifo[rx_rptr];
      rx_rptr = (uint8_t)((rx_rptr + 1) % sizeof(rx_fifo));
      rx_count--;
      return c;
    }
    return 0;                                /* no Rx char pending */
  }
  /* control read -> RR[pointer]; pointer auto-resets to 0 after the access */
  uint8_t p = ptr[ch];
  ptr[ch] = 0;
  if(scc_rx_dbg && rx_count > 0)
    fprintf(stderr, "[scc] ctrl read RR%u ch=%d wr1[%d]=%02x rx_count=%d\n", p, ch, ch, wr1[ch], rx_count);
  if(p == 3) {                               /* RR3 = chip-wide interrupt-pending */
    uint8_t r3 = 0;
    if(tx_int(wr1[1], tx_ip[1]))          r3 |= CHATxIP;
    if(tx_int(wr1[0], tx_ip[0]))          r3 |= CHBTxIP;
    if(rx_count > 0 && rx_int_en(wr1[1])) r3 |= CHARxIP;   /* console Rx avail (chanA) */
    if(rx_count > 0 && rx_int_en(wr1[0])) r3 |= CHBRxIP;   /* console Rx avail (chanB) */
    return r3;
  }
  /* RR0: Tx Buffer Empty + All Sent iff no char is shifting out; bit0 = Rx avail */
  uint8_t rr0 = tx_busy[ch] ? 0 : (RR0_TX_EMPTY | RR0_ALL_SENT);
  if(rx_count > 0) rr0 |= RR0_RX_AVAIL;
  return rr0;
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
  /* Serial-DUART source: a gated Rx-char-available (level), or a gated Tx-IP.
   * The Rx int is gated on WR1 Rx-int mode so it only asserts once the driver has
   * armed Rx interrupts (at the login getty open). Routed via IOC2 vmeistat[5] ->
   * cmeimask0 -> local0 LIO2 -> CPU IP2 (henry ioc.sv). */
  bool rx = (rx_count > 0) && (rx_int_en(wr1[0]) || rx_int_en(wr1[1]));
  return rx || tx_int(wr1[0], tx_ip[0]) || tx_int(wr1[1], tx_ip[1]);
}
