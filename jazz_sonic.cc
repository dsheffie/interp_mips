#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "jazz_sonic.hh"
#include "jazz.hh"

/* register indices (reg = off >> 2) */
enum {
  SONIC_CR = 0x00, SONIC_DCR = 0x01, SONIC_RCR = 0x02, SONIC_TCR = 0x03,
  SONIC_IMR = 0x04, SONIC_ISR = 0x05, SONIC_UTDA = 0x06, SONIC_CTDA = 0x07,
  SONIC_TPS = 0x08, SONIC_TFC = 0x09, SONIC_TSA0 = 0x0a, SONIC_TSA1 = 0x0b,
  SONIC_TFS = 0x0c, SONIC_URDA = 0x0d, SONIC_CRDA = 0x0e, SONIC_CRBA0 = 0x0f,
  SONIC_CRBA1 = 0x10, SONIC_RBWC0 = 0x11, SONIC_RBWC1 = 0x12, SONIC_EOBC = 0x13,
  SONIC_URRA = 0x14, SONIC_RSA = 0x15, SONIC_REA = 0x16, SONIC_RRP = 0x17,
  SONIC_RWP = 0x18, SONIC_TRBA0 = 0x19, SONIC_TRBA1 = 0x1a, SONIC_LLFA = 0x1f,
  SONIC_TTDA = 0x20, SONIC_CEP = 0x21, SONIC_CAP2 = 0x22, SONIC_CAP1 = 0x23,
  SONIC_CAP0 = 0x24, SONIC_CE = 0x25, SONIC_CDP = 0x26, SONIC_CDC = 0x27,
  SONIC_SR = 0x28, SONIC_WT0 = 0x29, SONIC_WT1 = 0x2a, SONIC_RSC = 0x2b,
  SONIC_CRCT = 0x2c, SONIC_FAET = 0x2d, SONIC_MPT = 0x2e, SONIC_MDT = 0x2f,
  SONIC_DCR2 = 0x3f,
};

static const uint16_t SONIC_CR_HTX = 0x0001, SONIC_CR_TXP = 0x0002, SONIC_CR_RXDIS = 0x0004;
static const uint16_t SONIC_CR_RXEN = 0x0008, SONIC_CR_STP = 0x0010, SONIC_CR_ST = 0x0020;
static const uint16_t SONIC_CR_RST = 0x0080, SONIC_CR_RRRA = 0x0100, SONIC_CR_LCAM = 0x0200;
static const uint16_t SONIC_CR_MASK = 0x03bf;
static const uint16_t SONIC_DCR_DW = 0x0020, SONIC_DCR_LBR = 0x2000, SONIC_DCR_EXBUS = 0x8000;
static const uint16_t SONIC_RCR_PRX = 0x0001, SONIC_RCR_LBK = 0x0002, SONIC_RCR_FAER = 0x0004;
static const uint16_t SONIC_RCR_CRCR = 0x0008, SONIC_RCR_LPKT = 0x0040, SONIC_RCR_BC = 0x0080;
static const uint16_t SONIC_RCR_MC = 0x0100, SONIC_RCR_LB0 = 0x0200, SONIC_RCR_LB1 = 0x0400;
static const uint16_t SONIC_RCR_AMC = 0x0800, SONIC_RCR_PRO = 0x1000, SONIC_RCR_BRD = 0x2000;
static const uint16_t SONIC_RCR_RNT = 0x4000;
static const uint16_t SONIC_TCR_PTX = 0x0001, SONIC_TCR_BCM = 0x0002, SONIC_TCR_CRSL = 0x0080;
static const uint16_t SONIC_TCR_NCRS = 0x0100, SONIC_TCR_CRCI = 0x2000, SONIC_TCR_PINT = 0x8000;
static const uint16_t SONIC_ISR_RBAE = 0x0010, SONIC_ISR_RBE = 0x0020, SONIC_ISR_RDE = 0x0040;
static const uint16_t SONIC_ISR_TC = 0x0080, SONIC_ISR_TXDN = 0x0200, SONIC_ISR_PKTRX = 0x0400;
static const uint16_t SONIC_ISR_PINT = 0x0800, SONIC_ISR_LCD = 0x1000;
static const uint16_t SONIC_DESC_EOL = 0x0001, SONIC_DESC_ADDR = 0xfffe;

/* reflected Ethernet CRC-32 (what QEMU gets from zlib's crc32), returned host-endian */
static uint32_t eth_crc32(const uint8_t *buf, uint32_t len) {
  uint32_t crc = 0xffffffffu;
  for(uint32_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for(int b = 0; b < 8; b++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320u : (crc >> 1);
    }
  }
  return ~crc;
}

jazz_sonic::jazz_sonic(jazz *jz, uint64_t ms_insns, const std::string &tap_ifname)
  : jz(jz), ms_insns(ms_insns) {
  tx_buffer.resize(0x10000);
  memset(regs, 0, sizeof(regs));
  memset(cam, 0, sizeof(cam));
  /* reset state (QEMU dp8393x_reset) */
  regs[SONIC_SR] = 0x0004;                        /* the only silicon revision Linux/MIPS accepts */
  regs[SONIC_CR] = SONIC_CR_RST | SONIC_CR_STP | SONIC_CR_RXDIS;
  regs[SONIC_TCR] = SONIC_TCR_NCRS | SONIC_TCR_PTX;

  const char *want = tap_ifname.empty() ? getenv("SONIC_TAP") : tap_ifname.c_str();
  if(want == nullptr or want[0] == 0) {
    fprintf(stderr, "[jazz] sonic: no tap (set SONIC_TAP=<ifname>); register model only\n");
    return;
  }
  int fd = ::open("/dev/net/tun", O_RDWR);
  if(fd < 0) {
    fprintf(stderr, "[jazz] sonic: open /dev/net/tun failed (need CAP_NET_ADMIN); register model only\n");
    return;
  }
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
  strncpy(ifr.ifr_name, want, IFNAMSIZ - 1);
  if(ioctl(fd, TUNSETIFF, &ifr) < 0) {
    fprintf(stderr, "[jazz] sonic: TUNSETIFF %s failed; register model only\n", want);
    ::close(fd);
    return;
  }
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  tap_fd = fd;
  fprintf(stderr, "[jazz] sonic: tap up on %s (fd %d)\n", ifr.ifr_name, tap_fd);
}

jazz_sonic::~jazz_sonic() {
  if(tap_fd >= 0) {
    ::close(tap_fd);
  }
}

/* paired-register address accessors (QEMU dp8393x_cdp etc.) */
uint32_t jazz_sonic::cdp() const { return (regs[SONIC_URRA] << 16) | regs[SONIC_CDP]; }
uint32_t jazz_sonic::crba() const { return (regs[SONIC_CRBA1] << 16) | regs[SONIC_CRBA0]; }
uint32_t jazz_sonic::crda() const { return (regs[SONIC_URDA] << 16) | (regs[SONIC_CRDA] & SONIC_DESC_ADDR); }
uint32_t jazz_sonic::rbwc() const { return (regs[SONIC_RBWC1] << 16) | regs[SONIC_RBWC0]; }
uint32_t jazz_sonic::rrp() const { return (regs[SONIC_URRA] << 16) | regs[SONIC_RRP]; }
uint32_t jazz_sonic::tsa() const { return (regs[SONIC_TSA1] << 16) | regs[SONIC_TSA0]; }
uint32_t jazz_sonic::ttda() const { return (regs[SONIC_UTDA] << 16) | (regs[SONIC_TTDA] & SONIC_DESC_ADDR); }
uint32_t jazz_sonic::wt() const { return (regs[SONIC_WT1] << 16) | regs[SONIC_WT0]; }

/* a descriptor word is 32 bits apart when DCR.DW is set, else 16; the build is little-endian */
uint16_t jazz_sonic::mem_get(uint32_t addr, int offset) {
  bool dw = (regs[SONIC_DCR] & SONIC_DCR_DW) != 0;
  uint8_t b[4] = {0, 0, 0, 0};
  uint32_t a = addr + (dw ? (offset << 2) : (offset << 1));
  jz->iommu_rw(a, b, dw ? 4 : 2, false);
  return (uint16_t)(b[0] | (b[1] << 8));
}

void jazz_sonic::mem_put(uint32_t addr, int offset, uint16_t val) {
  bool dw = (regs[SONIC_DCR] & SONIC_DCR_DW) != 0;
  uint8_t b[4] = {(uint8_t)(val & 0xff), (uint8_t)(val >> 8), 0, 0};
  uint32_t a = addr + (dw ? (offset << 2) : (offset << 1));
  jz->iommu_rw(a, b, dw ? 4 : 2, true);
}

void jazz_sonic::update_irq() {
  irq_level = (regs[SONIC_IMR] & regs[SONIC_ISR]) != 0;
}

void jazz_sonic::do_load_cam() {
  bool dw = (regs[SONIC_DCR] & SONIC_DCR_DW) != 0;
  int size = sizeof(uint16_t) * 4 * (dw ? 2 : 1);
  while(regs[SONIC_CDC] & 0x1f) {
    uint16_t index = mem_get(cdp(), 0) & 0xf;
    cam[index][0] = mem_get(cdp(), 1);
    cam[index][1] = mem_get(cdp(), 2);
    cam[index][2] = mem_get(cdp(), 3);
    regs[SONIC_CDC]--;
    regs[SONIC_CDP] += size;
  }
  regs[SONIC_CE] = mem_get(cdp(), 0);
  regs[SONIC_CR] &= ~SONIC_CR_LCAM;
  regs[SONIC_ISR] |= SONIC_ISR_LCD;
  update_irq();
}

void jazz_sonic::do_read_rra() {
  bool dw = (regs[SONIC_DCR] & SONIC_DCR_DW) != 0;
  int size = sizeof(uint16_t) * 4 * (dw ? 2 : 1);
  regs[SONIC_CRBA0] = mem_get(rrp(), 0);
  regs[SONIC_CRBA1] = mem_get(rrp(), 1);
  regs[SONIC_RBWC0] = mem_get(rrp(), 2);
  regs[SONIC_RBWC1] = mem_get(rrp(), 3);
  regs[SONIC_RRP] += size;
  if(regs[SONIC_RRP] == regs[SONIC_REA]) {
    regs[SONIC_RRP] = regs[SONIC_RSA];
  }
  if(regs[SONIC_RRP] == regs[SONIC_RWP]) {
    regs[SONIC_ISR] |= SONIC_ISR_RBE;
    update_irq();
  }
  last_rba_is_full = false;
}

void jazz_sonic::do_software_reset() {
  wt_running = false;
  regs[SONIC_CR] &= ~(SONIC_CR_LCAM | SONIC_CR_RRRA | SONIC_CR_TXP | SONIC_CR_HTX);
  regs[SONIC_CR] |= SONIC_CR_RST | SONIC_CR_RXDIS;
}

void jazz_sonic::set_next_tick() {
  if(regs[SONIC_CR] & SONIC_CR_STP) {
    wt_running = false;
    return;
  }
  /* WT counts down at 5 MHz; translate to the instruction time base */
  wt_running = true;
  wt_next = icnt + (uint64_t)wt() * ms_insns / 5000;
}

void jazz_sonic::do_transmit_packets() {
  while(true) {
    regs[SONIC_TTDA] = regs[SONIC_CTDA];
    int tx_len = 0;
    regs[SONIC_TCR] = mem_get(ttda(), 1) & 0xf000;
    regs[SONIC_TPS] = mem_get(ttda(), 2);
    regs[SONIC_TFC] = mem_get(ttda(), 3);
    regs[SONIC_TSA0] = mem_get(ttda(), 4);
    regs[SONIC_TSA1] = mem_get(ttda(), 5);
    regs[SONIC_TFS] = mem_get(ttda(), 6);
    if(regs[SONIC_TCR] & SONIC_TCR_PINT) {
      regs[SONIC_ISR] |= SONIC_ISR_PINT;
    }
    else {
      regs[SONIC_ISR] &= ~SONIC_ISR_PINT;
    }
    for(uint16_t i = 0; i < regs[SONIC_TFC]; ) {
      int len = regs[SONIC_TFS];
      if(tx_len + len > (int)tx_buffer.size()) {
        len = (int)tx_buffer.size() - tx_len;
      }
      jz->iommu_rw(tsa(), &tx_buffer[tx_len], len, false);
      tx_len += len;
      i++;
      if(i != regs[SONIC_TFC]) {
        regs[SONIC_TSA0] = mem_get(ttda(), 4 + 3 * i);
        regs[SONIC_TSA1] = mem_get(ttda(), 5 + 3 * i);
        regs[SONIC_TFS] = mem_get(ttda(), 6 + 3 * i);
      }
    }
    if(regs[SONIC_TCR] & SONIC_TCR_CRCI) {
      tx_len -= 4;                                 /* the frame carries its own FCS: drop it */
      if(tx_len < 0) {
        break;
      }
    }
    if(regs[SONIC_RCR] & (SONIC_RCR_LB1 | SONIC_RCR_LB0)) {
      regs[SONIC_TCR] |= SONIC_TCR_CRSL;           /* MAC loopback: feed it straight back to RX */
      if(can_receive()) {
        loopback_packet = true;
        receive(tx_buffer.data(), tx_len);
      }
    }
    else if(tap_fd >= 0 and tx_len >= 14) {
      ssize_t n = ::write(tap_fd, tx_buffer.data(), tx_len);
      (void)n;
    }
    regs[SONIC_TCR] |= SONIC_TCR_PTX;
    mem_put(ttda(), 0, regs[SONIC_TCR] & 0x0fff);
    if(not(regs[SONIC_CR] & SONIC_CR_HTX)) {
      regs[SONIC_CTDA] = mem_get(ttda(), 4 + 3 * regs[SONIC_TFC]);
      if(regs[SONIC_CTDA] & SONIC_DESC_EOL) {
        break;
      }
    }
  }
  regs[SONIC_CR] &= ~SONIC_CR_TXP;
  regs[SONIC_ISR] |= SONIC_ISR_TXDN;
  update_irq();
}

void jazz_sonic::do_command(uint16_t command) {
  if((regs[SONIC_CR] & SONIC_CR_RST) and not(command & SONIC_CR_RST)) {
    regs[SONIC_CR] &= ~SONIC_CR_RST;
    return;
  }
  regs[SONIC_CR] |= (command & SONIC_CR_MASK);
  if(command & SONIC_CR_TXP) {
    do_transmit_packets();
  }
  if(command & SONIC_CR_RXDIS) {
    regs[SONIC_CR] &= ~SONIC_CR_RXEN;
  }
  if(command & SONIC_CR_RXEN) {
    regs[SONIC_CR] &= ~SONIC_CR_RXDIS;
  }
  if(command & SONIC_CR_STP) {
    regs[SONIC_CR] &= ~SONIC_CR_ST;
    wt_running = false;
  }
  if(command & SONIC_CR_ST) {
    regs[SONIC_CR] &= ~SONIC_CR_STP;
    set_next_tick();
  }
  if(command & SONIC_CR_RST) {
    do_software_reset();
  }
  if(command & SONIC_CR_RRRA) {
    do_read_rra();
    regs[SONIC_CR] &= ~SONIC_CR_RRRA;
  }
  if(command & SONIC_CR_LCAM) {
    do_load_cam();
  }
}

bool jazz_sonic::can_receive() const {
  return (regs[SONIC_CR] & SONIC_CR_RXEN) != 0;
}

int jazz_sonic::receive_filter(const uint8_t *buf, int size) {
  (void)size;
  static const uint8_t bcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  if((regs[SONIC_RCR] & SONIC_RCR_PRO) and (buf[0] & 1) == 0) {
    return 0;
  }
  if((regs[SONIC_RCR] & SONIC_RCR_AMC) and (buf[0] & 1) == 1) {
    return SONIC_RCR_MC;
  }
  if((regs[SONIC_RCR] & SONIC_RCR_BRD) and memcmp(buf, bcast, 6) == 0) {
    return SONIC_RCR_BC;
  }
  for(int i = 0; i < 16; i++) {
    if((regs[SONIC_CE] & (1 << i)) and memcmp(buf, cam[i], 6) == 0) {
      return 0;
    }
  }
  return -1;
}

void jazz_sonic::receive(const uint8_t *buf, uint32_t pkt_size) {
  regs[SONIC_RCR] &= ~(SONIC_RCR_PRX | SONIC_RCR_LBK | SONIC_RCR_FAER | SONIC_RCR_CRCR |
                       SONIC_RCR_LPKT | SONIC_RCR_BC | SONIC_RCR_MC);
  if(last_rba_is_full) {
    return;
  }
  int rx_len = pkt_size + 4;
  int padded_len = (regs[SONIC_DCR] & SONIC_DCR_DW) ? (((rx_len - 1) | 3) + 1) : (((rx_len - 1) | 1) + 1);
  if(padded_len > (int)rbwc() * 2) {
    regs[SONIC_ISR] |= SONIC_ISR_RBAE;
    update_irq();
    regs[SONIC_RCR] |= SONIC_RCR_LPKT;
    goto done;
  }
  {
    int packet_type = receive_filter(buf, pkt_size);
    if(packet_type < 0) {
      return;                                      /* not for us */
    }
    if(regs[SONIC_LLFA] & SONIC_DESC_EOL) {        /* were we out of descriptors? */
      regs[SONIC_LLFA] = mem_get(crda(), 5);
      if(regs[SONIC_LLFA] & SONIC_DESC_EOL) {
        return;                                    /* still EOL: drop */
      }
      mem_put(crda(), 6, 0x0000);
      regs[SONIC_CRDA] = regs[SONIC_LLFA];
      regs[SONIC_ISR] |= SONIC_ISR_PKTRX;
    }
    regs[SONIC_TRBA1] = regs[SONIC_CRBA1];
    regs[SONIC_TRBA0] = regs[SONIC_CRBA0];
    uint32_t crc = eth_crc32(buf, pkt_size);       /* stored little-endian after the frame */
    uint8_t crcb[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
    uint32_t address = crba();
    jz->iommu_rw(address, (uint8_t *)buf, pkt_size, true);
    address += pkt_size;
    jz->iommu_rw(address, crcb, 4, true);
    address += 4;
    if(rx_len < padded_len) {
      uint8_t pad[3] = {0xff, 0xff, 0xff};
      jz->iommu_rw(address, pad, padded_len - rx_len, true);
      address += padded_len - rx_len;
    }
    regs[SONIC_CRBA1] = address >> 16;
    regs[SONIC_CRBA0] = address & 0xffff;
    uint32_t available = rbwc() - (padded_len >> 1);
    regs[SONIC_RBWC1] = available >> 16;
    regs[SONIC_RBWC0] = available & 0xffff;
    if(rbwc() < regs[SONIC_EOBC]) {
      regs[SONIC_RCR] |= SONIC_RCR_LPKT;
    }
    regs[SONIC_RCR] |= packet_type | SONIC_RCR_PRX;
    if(loopback_packet) {
      regs[SONIC_RCR] |= SONIC_RCR_LBK;
      loopback_packet = false;
    }
    mem_put(crda(), 0, regs[SONIC_RCR]);           /* status */
    mem_put(crda(), 1, rx_len);                    /* byte count */
    mem_put(crda(), 2, regs[SONIC_TRBA0]);
    mem_put(crda(), 3, regs[SONIC_TRBA1]);
    mem_put(crda(), 4, regs[SONIC_RSC]);           /* seq no */
    regs[SONIC_LLFA] = mem_get(crda(), 5);
    if(regs[SONIC_LLFA] & SONIC_DESC_EOL) {
      regs[SONIC_ISR] |= SONIC_ISR_RDE;
    }
    else {
      mem_put(crda(), 6, 0x0000);
      regs[SONIC_CRDA] = regs[SONIC_LLFA];
      regs[SONIC_ISR] |= SONIC_ISR_PKTRX;
    }
    update_irq();
    regs[SONIC_RSC] = (regs[SONIC_RSC] & 0xff00) | ((regs[SONIC_RSC] + 1) & 0x00ff);
  }
done:
  if(regs[SONIC_RCR] & SONIC_RCR_LPKT) {
    if(regs[SONIC_RRP] == regs[SONIC_RWP]) {
      last_rba_is_full = true;
    }
    else {
      do_read_rra();
    }
  }
}

void jazz_sonic::poll_tap() {
  if(tap_fd < 0 or not can_receive()) {
    return;
  }
  uint8_t buf[2048];
  for(int i = 0; i < 16; i++) {                    /* bounded drain per poll */
    ssize_t n = ::read(tap_fd, buf, sizeof(buf));
    if(n <= 0) {
      break;
    }
    if(n >= 14) {
      receive(buf, (uint32_t)n);
    }
  }
}

void jazz_sonic::tick(uint64_t ic) {
  icnt = ic;
  if(wt_running and icnt >= wt_next) {             /* watchdog underflow (QEMU dp8393x_watchdog) */
    regs[SONIC_WT1] = 0xffff;
    regs[SONIC_WT0] = 0xffff;
    regs[SONIC_ISR] |= SONIC_ISR_TC;
    update_irq();
    set_next_tick();
  }
  if(icnt >= next_poll) {
    next_poll = icnt + ms_insns;
    poll_tap();
  }
}

uint16_t jazz_sonic::read(uint32_t off) {
  int reg = (off >> 2) & 0x3f;
  switch(reg)
    {
    case SONIC_CAP2:
    case SONIC_CAP1:
    case SONIC_CAP0:
      return (regs[SONIC_CR] & SONIC_CR_RST) ? cam[regs[SONIC_CEP] & 0xf][SONIC_CAP0 - reg] : 0;
    default:
      return regs[reg];
    }
}

void jazz_sonic::write(uint32_t off, uint16_t val) {
  int reg = (off >> 2) & 0x3f;
  switch(reg)
    {
    case SONIC_CR:
      do_command(val);
      break;
    case SONIC_CAP2:                               /* read-only */
    case SONIC_CAP1:
    case SONIC_CAP0:
    case SONIC_SR:
    case SONIC_MDT:
      break;
    case SONIC_DCR:                                /* writable only in reset mode */
      if(regs[SONIC_CR] & SONIC_CR_RST) {
        regs[reg] = val & 0xbfff;
      }
      break;
    case SONIC_DCR2:
      if(regs[SONIC_CR] & SONIC_CR_RST) {
        regs[reg] = val & 0xf017;
      }
      break;
    case SONIC_TCR:
      regs[reg] = val & 0xf000;
      break;
    case SONIC_RCR:
      regs[reg] = val & 0xffe0;
      break;
    case SONIC_IMR:
      regs[reg] = val & 0x7fff;
      update_irq();
      break;
    case SONIC_ISR:                                /* write 1 to clear */
      val &= regs[reg];
      regs[reg] &= ~val;
      if(val & SONIC_ISR_RBE) {
        do_read_rra();
      }
      update_irq();
      break;
    case SONIC_RSA:
    case SONIC_REA:
    case SONIC_RRP:
    case SONIC_RWP:
      regs[reg] = (regs[SONIC_DCR] & SONIC_DCR_DW) ? (val & 0xfffc) : (val & 0xfffe);
      break;
    case SONIC_CRCT:
    case SONIC_FAET:
    case SONIC_MPT:
      regs[reg] = val ^ 0xffff;
      break;
    default:
      regs[reg] = val;
      break;
    }
  if(reg == SONIC_WT0 or reg == SONIC_WT1) {
    set_next_tick();
  }
}
