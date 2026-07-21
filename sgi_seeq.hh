#ifndef __sgi_seeq__
#define __sgi_seeq__

#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>
#include <string>

struct state_t;

/*
 * Seeq 8003 / 80C03 Ethernet Data Link Controller (behavioral model), the
 * ethernet analog of sgi_scsi (WD33C93 + disk).  On IP22 the CPU programs the
 * Seeq through 8 byte-registers, word-spaced, in the HPC3 ENET device window
 * (base PA 0x1fb80000 + 0x50000).  Two HPC3 ENET-DMA channels (TX and RX) move
 * the frame data between guest DRAM and this device, byte-at-a-time via
 * tx_dma_w()/rx_dma_r() -- driven by the descriptor-walk FSM in sgi_hpc
 * (mirrors scsi_run_dma).
 *
 * Register model (regs 0-7, from IRIX if_ec2.c / seeq.h, cross-checked against
 * the iris emulator's seeq8003.rs which boots IRIX networking):
 *   0-5  station address (write, bank 0) / read-back = 80C03 collision counters
 *        + EDLC flags.  read reg0 == 0 -> driver identifies the SGI 80C03 EDLC;
 *        read reg5 == NO_SQE -> carrier present (normal 10base-T).  The write
 *        bank is selected by tx_cmd bits 6:5 (station / mcast-lsb / mcast-msb).
 *   6    RX_REG: read = RX status, write = RX command (match mode + int enables)
 *   7    TX_REG: read = TX status, write = TX command (bank sel + int enables)
 *
 * RX DMA buffer layout the driver expects: [2 pad][ethernet frame][1 status].
 * The status byte (RX_STATUS_GOOD = GOOD|END = 0x30) is appended as the LAST
 * DMA byte, not read from a register.
 *
 * Backend: a host TAP device (open /dev/net/tun, IFF_TAP|IFF_NO_PI) -- an L2
 * ethernet-frame conduit.  TX frames are write()'d to the tap; inbound frames
 * are read() from the tap, address-filtered, and queued for RX DMA.  The host
 * (dev box now / FPGA PS later, both Linux) bridges or NATs the tap to the real
 * network -- identical code both places.
 */
class sgi_seeq {
public:
  /* register offsets (A2:A0) */
  enum { R_STA0 = 0, R_STA5 = 5, R_RX = 6, R_TX = 7 };
private:
  state_t *s;
  int      tap_fd = -1;

  /* Seeq register state (mirrors iris seeq8003.rs SeeqState) */
  uint8_t  rx_cmd  = 0;
  uint8_t  rx_stat = 0x80;                 /* OLD (stale) at reset */
  uint8_t  tx_cmd  = 0;
  uint8_t  tx_stat = 0x88;                 /* OLD | SUCCESS (ready) */
  uint8_t  station_addr[6] = {0};
  uint8_t  mcast_lsb[6] = {0};
  uint8_t  mcast_msb[2] = {0};
  uint8_t  pktgap = 0, ctl = 0;
  bool     intpend = false;

  /* TX: bytes fed by the HPC3 ENET TX DMA accumulate here until the chain ends */
  std::vector<uint8_t> tx_frame;

  /* RX: inbound frames pulled from the tap, address-filtered, awaiting DMA.
   * rx_cur holds the [pad][pad][frame][status] byte stream being DMA'd out. */
  std::deque<std::vector<uint8_t>> rx_queue;
  std::vector<uint8_t> rx_cur;
  size_t   rx_pos = 0;

  bool     address_filter(const uint8_t *frame, size_t len) const;
  void     raise_interrupt();

public:
  sgi_seeq(state_t *s, const std::string &tap_ifname = "");
  ~sgi_seeq();
  bool ok() const { return tap_fd >= 0; }

  /* CPU PIO to the 8 Seeq registers (port = register index 0..7). */
  void    pio_w(uint32_t reg, uint8_t v);
  uint8_t pio_r(uint32_t reg);

  /* HPC3 ENET TX DMA channel: guest DRAM -> device. tx_dma_w() accumulates a
   * frame byte; tx_flush() (called at the chain end / EOX) transmits it. */
  void    tx_dma_w(uint8_t v);
  void    tx_flush();

  /* HPC3 ENET RX DMA channel: device -> guest DRAM. rx_avail() is DRQ (a frame
   * is staged); rx_dma_r() returns the next [pad|frame|status] byte and sets
   * last on the appended status byte. */
  bool    rx_avail();
  uint8_t rx_dma_r(bool &last);

  /* pull any inbound frames off the tap into rx_queue (non-blocking). Call per
   * device step, like the SCSI poll. */
  void    poll();

  void reset();
  bool intrq_pending() const { return intpend; }
  /* non-destructive status peeks for the HPC3 rx_ctrl/tx_ctrl STAT bits (unlike
   * pio_r on reg 6/7, these do NOT mark the status OLD). */
  uint8_t rstat_peek() const { return rx_stat; }
  uint8_t tstat_peek() const { return tx_stat; }
};

#endif
