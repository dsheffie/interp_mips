#include "sgi_seeq.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>

/* ---- Seeq register bits (seeq.h / iris seeq8003.rs) ------------------------ */
namespace {
  /* RX command (write reg 6): match mode in bits 7:6, int enables in bits 4:0 */
  constexpr uint8_t RC_MATCH_MASK   = 0xc0;
  constexpr uint8_t RC_MATCH_NONE   = 0x00;  /* receiver disabled */
  constexpr uint8_t RC_MATCH_PROMIS = 0x40;  /* receive all */
  constexpr uint8_t RC_MATCH_STABC  = 0x80;  /* station + broadcast */
  constexpr uint8_t RC_MATCH_STAMC  = 0xc0;  /* station + broadcast + multicast */

  /* RX status (read reg 6) */
  constexpr uint8_t RS_OLD  = 0x80;          /* 1 = already read (stale) */
  constexpr uint8_t RS_GOOD = 0x20;
  constexpr uint8_t RS_END  = 0x10;

  /* TX command (write reg 7): bank select in bits 6:5 */
  constexpr uint8_t XC_BANK_MASK = 0x60;
  constexpr uint8_t XC_BANK0     = 0x00;     /* station address */
  constexpr uint8_t XC_BANK1     = 0x20;     /* multicast lsb */
  constexpr uint8_t XC_BANK2     = 0x40;     /* multicast msb / pktgap / ctl */

  /* TX status (read reg 7) */
  constexpr uint8_t XS_OLD     = 0x80;
  constexpr uint8_t XS_SUCCESS = 0x08;

  /* 80C03 read-mode reg 5 flags: NO_SQE set -> carrier present, no heartbeat */
  constexpr uint8_t EDLC_NO_SQE = 0x01;

  /* status byte the Seeq appends to the RX DMA'd frame */
  constexpr uint8_t RX_STATUS_GOOD = RS_GOOD | RS_END;  /* 0x30 */

  constexpr size_t ETH_MIN = 60, ETH_MAX = 1514;
}

sgi_seeq::sgi_seeq(state_t *s_, const std::string &tap_ifname) : s(s_) {
  const char *want = tap_ifname.empty() ? getenv("SEEQ_TAP") : tap_ifname.c_str();
  if(!want) {
    /* no tap requested: register model is still live (driver init / EDLC probe
     * works), just no real frame I/O. */
    fprintf(stderr, "sgi_seeq: no tap (set SEEQ_TAP=<ifname>); register model only\n");
    return;
  }
  int fd = ::open("/dev/net/tun", O_RDWR);
  if(fd < 0) {
    fprintf(stderr, "sgi_seeq: open /dev/net/tun: %s (need CAP_NET_ADMIN); register model only\n",
            strerror(errno));
    return;
  }
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;             /* L2 frames, no packet-info prefix */
  strncpy(ifr.ifr_name, want, IFNAMSIZ - 1);
  if(ioctl(fd, TUNSETIFF, &ifr) < 0) {
    fprintf(stderr, "sgi_seeq: TUNSETIFF %s: %s; register model only\n", want, strerror(errno));
    ::close(fd);
    return;
  }
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  tap_fd = fd;
  fprintf(stderr, "sgi_seeq: tap up on %s (fd %d)\n", ifr.ifr_name, tap_fd);
}

sgi_seeq::~sgi_seeq() {
  if(tap_fd >= 0) { ::close(tap_fd); }
}

void sgi_seeq::reset() {
  rx_cmd = 0; rx_stat = RS_OLD;
  tx_cmd = 0; tx_stat = XS_OLD | XS_SUCCESS;
  ctl = 0;
  intpend = false;
  tx_frame.clear();
  rx_queue.clear();
  rx_cur.clear();
  rx_pos = 0;
}

/* raise/lower INTRQ.  Interrupt when a status reg is NEW (OLD=0) and a matching
 * enable bit is set (mirrors iris raise_interrupt; DMA-channel xie is folded in
 * separately by sgi_hpc). */
void sgi_seeq::raise_interrupt() {
  bool rx_irq = !(rx_stat & RS_OLD) && ((rx_cmd & 0x1f) & (rx_stat & 0x1f));
  bool tx_irq = !(tx_stat & XS_OLD) && ((tx_cmd & 0x0f) & (tx_stat & 0x0f));
  intpend = rx_irq || tx_irq;
}

bool sgi_seeq::address_filter(const uint8_t *frame, size_t len) const {
  if(len < 6) { return false; }
  static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
  bool is_bcast = memcmp(frame, bcast, 6) == 0;
  bool is_sta   = memcmp(frame, station_addr, 6) == 0;
  bool is_multi = (frame[0] & 1) != 0;
  switch(rx_cmd & RC_MATCH_MASK) {
  case RC_MATCH_NONE:   return false;
  case RC_MATCH_PROMIS: return true;
  case RC_MATCH_STABC:  return is_bcast || is_sta;
  case RC_MATCH_STAMC:  return is_bcast || is_sta || is_multi;
  default:              return is_bcast || is_sta;
  }
}

uint8_t sgi_seeq::pio_r(uint32_t reg) {
  uint8_t v = 0;
  switch(reg & 7) {
  case R_STA0: case 1: case 2: case 3: case 4: case R_STA5:
    /* 80C03 read mode: regs 0-1 = coll_xmit (0 -> SGI EDLC), reg5 = flags. */
    v = ((reg & 7) == 5) ? EDLC_NO_SQE : 0x00;
    break;
  case R_RX:
    v = rx_stat;
    rx_stat |= RS_OLD;          /* reading marks it stale */
    raise_interrupt();
    break;
  case R_TX:
    v = tx_stat;
    tx_stat |= XS_OLD;
    raise_interrupt();
    break;
  }
  return v;
}

void sgi_seeq::pio_w(uint32_t reg, uint8_t val) {
  switch(reg & 7) {
  case R_STA0: case 1: case 2: case 3: case 4: case R_STA5: {
    int idx = reg & 7;
    switch(tx_cmd & XC_BANK_MASK) {          /* write bank selected by tx_cmd */
    case XC_BANK0: station_addr[idx] = val; break;
    case XC_BANK1: mcast_lsb[idx] = val; break;
    case XC_BANK2:
      if(idx == 0 || idx == 1) { mcast_msb[idx] = val; }
      else if(idx == 2)        { pktgap = val; }
      else if(idx == 3)        { ctl = val; }
      break;
    default: break;
    }
    break;
  }
  case R_RX: rx_cmd = val; raise_interrupt(); break;
  case R_TX: tx_cmd = val; raise_interrupt(); break;
  }
}

/* HPC3 ENET TX DMA feeds guest-DRAM bytes here; the chain end calls tx_flush(). */
void sgi_seeq::tx_dma_w(uint8_t v) {
  tx_frame.push_back(v);
}

void sgi_seeq::tx_flush() {
  if(tx_frame.empty()) { return; }
  if(tap_fd >= 0 && tx_frame.size() >= 14) {
    ssize_t n = ::write(tap_fd, tx_frame.data(), tx_frame.size());
    (void)n;
  }
  static const bool dbg = getenv("SEEQ_DBG") != nullptr;
  if(dbg) fprintf(stderr, "sgi_seeq: TX %zu bytes to %02x:%02x:%02x:%02x:%02x:%02x\n",
                  tx_frame.size(), tx_frame[0], tx_frame[1], tx_frame[2],
                  tx_frame[3], tx_frame[4], tx_frame[5]);
  tx_frame.clear();
  tx_stat = XS_SUCCESS;         /* NEW status (OLD cleared) -> may raise INTGOOD */
  raise_interrupt();
}

/* Non-blocking drain of the tap into rx_queue, applying the address filter. */
void sgi_seeq::poll() {
  if(tap_fd < 0) { return; }
  uint8_t buf[2048];
  for(;;) {
    ssize_t n = ::read(tap_fd, buf, sizeof(buf));
    if(n <= 0) { break; }                    /* EAGAIN / no frame */
    if((size_t)n < 14) { continue; }
    static const bool dbg = getenv("SEEQ_DBG") != nullptr;
    if(!address_filter(buf, (size_t)n)) {
      if(dbg) fprintf(stderr, "sgi_seeq: RX drop (filter) %zd bytes, rx_cmd=%02x\n", n, rx_cmd);
      continue;
    }
    if(dbg) fprintf(stderr, "sgi_seeq: RX queue %zd bytes to %02x:%02x:%02x:%02x:%02x:%02x\n",
                    n, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    rx_queue.emplace_back(buf, buf + n);
  }
}

/* DRQ for the RX DMA channel: a frame is staged (or one is available to stage). */
bool sgi_seeq::rx_avail() {
  if(rx_pos < rx_cur.size()) { return true; }
  if(rx_queue.empty()) { return false; }
  /* stage the next frame as [pad][pad][frame...][status] */
  std::vector<uint8_t> &f = rx_queue.front();
  size_t len = f.size();
  if(len < ETH_MIN) { len = ETH_MIN; }       /* pad runt to min ethernet */
  rx_cur.clear();
  rx_cur.reserve(2 + len + 1);
  rx_cur.push_back(0); rx_cur.push_back(0);  /* 2-byte pad */
  rx_cur.insert(rx_cur.end(), f.begin(), f.end());
  rx_cur.resize(2 + len, 0);                 /* zero-fill the runt pad */
  rx_cur.push_back(RX_STATUS_GOOD);          /* appended status byte */
  rx_queue.pop_front();
  rx_pos = 0;
  return true;
}

uint8_t sgi_seeq::rx_dma_r(bool &last) {
  if(rx_pos >= rx_cur.size()) { last = true; return 0; }
  uint8_t v = rx_cur[rx_pos++];
  last = (rx_pos >= rx_cur.size());
  if(last) {
    rx_stat = RS_GOOD | RS_END;              /* NEW status -> may raise INTEOF */
    raise_interrupt();
  }
  return v;
}
