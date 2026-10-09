#ifndef __JAZZ_SONIC_HH__
#define __JAZZ_SONIC_HH__

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class jazz;

/* jazz_sonic: the National Semiconductor DP83932 "SONIC" Ethernet controller at 0x80001000,
 * rc4030 local interrupt source 4, as on the MIPS Magnum / Jazz.  A synchronous port of QEMU
 * hw/net/dp8393x.c (see also Linux drivers/net/ethernet/natsemi/sonic.c); it_shift = 2 so the
 * 16-bit registers sit 4 bytes apart, and it reaches its descriptor rings / buffers through the
 * rc4030 IOMMU (jazz::iommu_rw), exactly like the ESP.  Little-endian build only.
 *
 * The register file + CAM load + resource/descriptor-ring walks + TX/RX + watchdog + interrupt
 * logic all match QEMU.  The host conduit is a Linux TAP (env SONIC_TAP=<ifname>, like the SGI
 * SEEQ model): TX frames are written to the tap, inbound frames are read, address-filtered and
 * DMA'd into the receive ring.  With no tap the register model is still fully live (so the NT /
 * Linux driver binds and starts), TX is dropped and nothing is received. */
class jazz_sonic {
public:
  jazz_sonic(jazz *jz, uint64_t ms_insns, const std::string &tap_ifname);
  ~jazz_sonic();
  uint16_t read(uint32_t off);                   /* off = byte offset within the 0x100 reg window */
  void write(uint32_t off, uint16_t val);
  void tick(uint64_t icnt);                      /* watchdog + tap drain */
  bool irq() const { return irq_level; }
private:
  jazz *jz;
  uint64_t ms_insns, icnt = 0;
  uint16_t regs[64];
  uint16_t cam[16][3];
  bool irq_level = false;
  bool last_rba_is_full = false, loopback_packet = false;
  std::vector<uint8_t> tx_buffer;
  /* watchdog (WT0/WT1 countdown at 5 MHz) */
  bool wt_running = false;
  uint64_t wt_next = 0;
  /* host tap */
  int tap_fd = -1;
  uint64_t next_poll = 0;

  uint32_t cdp() const;
  uint32_t crba() const;
  uint32_t crda() const;
  uint32_t rbwc() const;
  uint32_t rrp() const;
  uint32_t tsa() const;
  uint32_t ttda() const;
  uint32_t wt() const;
  uint16_t mem_get(uint32_t addr, int offset);
  void mem_put(uint32_t addr, int offset, uint16_t val);
  void update_irq();
  void do_load_cam();
  void do_read_rra();
  void do_software_reset();
  void do_transmit_packets();
  void do_command(uint16_t command);
  void set_next_tick();
  int receive_filter(const uint8_t *buf, int size);
  void receive(const uint8_t *buf, uint32_t pkt_size);
  bool can_receive() const;
  void poll_tap();
};

#endif
