#ifndef __sgi_scc__
#define __sgi_scc__

#include <cstdint>
#include <cstddef>

struct state_t;

/* Zilog Z85230 / SCC85230 (SGI IP22 IOC2 serial): TX path + the Tx interrupt.
 *
 * Tx-interrupt model cross-validated against four sources that all agree (MAME
 * z80scc.cpp, the IRIS Rust Indy emulator z85c30.rs, and the Linux ip22zilog +
 * NetBSD zstty drivers):
 *
 *   - Tx-IP is a stored one-shot set ONLY on transmit COMPLETION (a char finishes
 *     shifting out), never on the data write.
 *   - Writing the next data byte CLEARS Tx-IP (re-arm) -- this is the ISR's normal
 *     "send next char" deassert, and it is what makes IP2 go LOW so the kernel can
 *     eret back to userspace instead of instantly re-taking the interrupt (the
 *     storm).  WR0=RES_Tx_P (0x28) also clears it.
 *   - The pending Tx int is gated LIVE on WR1.TxINT_ENAB (so clearing TxIE
 *     deasserts immediately).
 *
 * Real transmit timing is required so IP2 is genuinely low between completions:
 * a written char is "shifting out" (tx_busy, RR0 Tx-Buffer-Empty clear) for
 * TX_DRAIN_TICKS instruction ticks, then completes (Tx-IP set).  The byte is
 * echoed to stdout immediately on write (order preserved).  int_pending() (the
 * SCC INT line) -> IOC2 vmeistat[5] -> local0 LIO2 -> CPU IP2.  RX not modeled.
 *
 * Window: +0x0 ctrl B  +0x4 data B  +0x8 ctrl A  +0xc data A (z80scc ab_dc). */
class sgi_scc {
  state_t *s;
  uint8_t  ptr[2]      = {0, 0};        /* RR/WR pointer per channel (0=B, 1=A) */
  uint8_t  wr1[2]      = {0, 0};        /* WR1 per channel (TxINT_ENAB = bit1) */
  uint8_t  wr9         = 0;             /* WR9 (chip-wide; MIE = bit3) */
  bool     tx_busy[2]  = {false, false};/* a char is shifting out (buffer full) */
  uint64_t drain_at[2] = {0, 0};        /* tick at which the shifting char completes */
  bool     tx_ip[2]    = {false, false};/* Tx-buffer-empty int pending (set on completion) */
  uint64_t clk         = 0;             /* instruction-tick clock */
public:
  sgi_scc(state_t *s) : s(s) {}
  uint8_t read(uint32_t offs);          /* offs within the 16-byte SCC window */
  void    write(uint32_t offs, uint8_t b);
  void    tick();                       /* advance TX timing; call once per insn */
  bool    int_pending();                /* SCC INT line: any channel TxIE & Tx-IP */
};

#endif
