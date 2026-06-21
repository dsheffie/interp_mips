#ifndef __sgi_scc__
#define __sgi_scc__

#include <cstdint>
#include <cstddef>

struct state_t;

/* Zilog Z8530 / SCC85230 (SGI IP22 IOC2 serial).  TX path + the Tx-buffer-empty
 * INTERRUPT, which the Linux ip22zilog tty driver needs to drain its write FIFO
 * (kernel console writes are polled and worked already; userspace /dev/console
 * output is interrupt-driven and did NOT, because the SCC raised no interrupt).
 *
 * 4 byte-wide registers at 4-byte spacing, z80scc "ab_dc" order:
 *   +0x0 ctrl B  +0x4 data B  +0x8 ctrl A  +0xc data A
 * Control access uses the Z8530 two-step pointer: a write to control when the
 * pointer is 0 is WR0 (low 3 bits set the pointer; bits[5:3] are a command,
 * e.g. RES_Tx_P; "point high" adds 8); the next control access reads/writes
 * RR/WR<pointer>, then the pointer resets to 0.
 *
 * TX model (instant transmit): a data write emits the byte to stdout; the Tx
 * buffer is then immediately empty, so if Tx-int is enabled (WR1.TxINT_ENAB) and
 * the master int is enabled (WR9.MIE) we latch a per-channel Tx-IP.  The kernel
 * ISR reads RR3 (chip-wide int-pending: CHATxIP/CHBTxIP), sends the next char
 * (re-arming Tx-IP) or, when its FIFO drains, issues RES_Tx_P (WR0) which clears
 * Tx-IP.  int_pending() (MIE & any Tx-IP) is the SCC INT line -> IOC2 vmeistat[5].
 *
 * Base 0x1fbd9830 (IOC2 reg 0x0c).  RX not modeled (TX-only console). */
class sgi_scc {
  state_t *s;
  uint8_t  ptr[2]   = {0, 0};        /* RR/WR pointer per channel (0=B, 1=A) */
  uint8_t  wr1[2]   = {0, 0};        /* WR1 per channel (TxINT_ENAB = bit1) */
  uint8_t  wr9      = 0;             /* WR9 (chip-wide; MIE = bit3) */
  bool     tx_ip[2] = {false, false};/* latched Tx-buffer-empty int pending */
public:
  sgi_scc(state_t *s) : s(s) {}
  uint8_t read(uint32_t offs);          /* offs within the 16-byte SCC window */
  void    write(uint32_t offs, uint8_t b);
  bool    int_pending();                /* SCC INT line: MIE & any channel Tx-IP */
};

#endif
