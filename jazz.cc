#include <vector>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "jazz.hh"
#include "jazz_esp.hh"
#include "jazz_sonic.hh"
#include "interpret.hh"

/* local interrupt sources (bit = rc4030 jazz irq; Linux JAZZ_*_IRQ - JAZZ_IRQ_START) */
static const int SRC_SONIC = 4, SRC_FDC = 1, SRC_SCSI = 5, SRC_KBD = 6, SRC_MOUSE = 7, SRC_COM1 = 8, SRC_COM2 = 9;

jazz::jazz(state_t *s, const std::string &nvram_file, uint64_t ms_insns, const std::string &disk,
           bool disk_write, const std::string &net_tap)
  : s(s), ms_insns(ms_insns) {
  memset(r4030, 0, sizeof(r4030));
  memset(cmos, 0, sizeof(cmos));
  memset(palette, 0, sizeof(palette));
  /* R4030 reset values (QEMU rc4030_reset): config, revision, refresh, NVRAM protect, remote speed */
  auto put32 = [&](uint32_t o, uint32_t v) {
    for(int i = 0; i < 4; i++) {
      r4030[o + i] = (v >> (8*i)) & 0xff;
    }
  };
  put32(0x000, 0x410);
  put32(0x008, 1);
  put32(0x210, 0x18186);
  put32(0x220, 7);
  for(uint32_t o = 0x070; o <= 0x0e0; o += 8) {
    put32(o, 7);
  }
  /* DS1287: binary mode, 24 h (the battery-backed state NT/the ROM expect, see handoff §3c) */
  cmos[0xa] = 0x26;
  cmos[0xb] = 0x06;
  cmos[0xd] = 0x80;
  rtc_base_sec = time(nullptr);
  /* DS1225 NVRAM: map the file shared so the firmware's environment writes persist */
  nvram_fd = open(nvram_file.c_str(), O_RDWR);
  if(nvram_fd < 0) {
    fprintf(stderr, "[jazz] cannot open nvram %s\n", nvram_file.c_str());
    exit(-1);
  }
  struct stat st;
  fstat(nvram_fd, &st);
  if(st.st_size < 8192) {
    fprintf(stderr, "[jazz] nvram %s is %ld bytes, need 8192\n", nvram_file.c_str(), (long)st.st_size);
    exit(-1);
  }
  nvram = reinterpret_cast<uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, nvram_fd, 0));
  esp = new jazz_esp(this, disk, disk_write);
  sonic = new jazz_sonic(this, ms_insns, net_tap);
}

jazz::~jazz() {
  delete esp;
  delete sonic;
  if(nvram != nullptr) {
    munmap(nvram, 8192);
  }
  if(nvram_fd >= 0) {
    close(nvram_fd);
  }
}

bool jazz::is_memory(uint64_t pa) const {
  return (pa >= 0x40000000ULL and pa < 0x40800000ULL) or     /* G364 VRAM */
    (pa >= 0x1fc00000ULL and pa < 0x20000000ULL) or          /* PROM */
    (pa >= 0xfff00000ULL and pa < 0x100000000ULL);           /* PROM alias */
}

void jazz::log_unhandled(const char *what, uint64_t pa, uint64_t v, int sz) {
  if(logged.insert(pa & ~0xfULL).second) {
    fprintf(stderr, "[jazz] unhandled %s pa=%08llx sz=%d v=%llx pc=%08x icnt=%llu\n", what,
            (unsigned long long)pa, sz, (unsigned long long)v, (uint32_t)s->pc,
            (unsigned long long)s->icnt);
  }
}

/* JAZZ_MMIO=1: log every device access (compare with the J0 QEMU memory_region_ops trace) */
static const bool g_jazz_mmio = getenv("JAZZ_MMIO") != nullptr;

uint64_t jazz::read(uint64_t pa, int sz) {
  uint64_t v = read_dev(pa, sz);
  if(g_jazz_mmio) {
    fprintf(stderr, "[mmio] r %08llx sz=%d v=%llx pc=%08x icnt=%llu\n", (unsigned long long)pa, sz,
            (unsigned long long)v, (uint32_t)s->pc, (unsigned long long)s->icnt);
  }
  return v;
}

uint64_t jazz::read_dev(uint64_t pa, int sz) {
  /* 32-bit R4030 registers with read side effects are handled as words */
  if(pa >= 0x80000000ULL and pa < 0x80001000ULL and sz >= 4) {
    uint32_t o = (uint32_t)(pa - 0x80000000ULL);
    if(o == 0x230) {
      timer_pending = false;                     /* interval timer: reading acknowledges */
      return 0;
    }
    uint64_t v = r4030_read32(o);
    if(sz == 8) {
      v |= uint64_t(r4030_read32(o + 4)) << 32;
    }
    if(o == 0x058 and (uint32_t)v == 0xffffffffu) {
      r4030_put32(0x058, 0);                     /* I/O cache byte mask: a full mask reads once, then clears */
    }
    return v;
  }
  if(pa >= 0x80001000ULL and pa < 0x80001100ULL and sz >= 2) {
    uint16_t rv = sonic->read((uint32_t)(pa - 0x80001000ULL));   /* 16-bit SONIC registers (it_shift 2) */
    update_isr();
    return rv;
  }
  uint64_t v = 0;
  for(int i = 0; i < sz; i++) {
    v |= uint64_t(read8(pa + i)) << (8*i);
  }
  return v;
}

void jazz::write(uint64_t pa, uint64_t v, int sz) {
  if(g_jazz_mmio) {
    fprintf(stderr, "[mmio] w %08llx sz=%d v=%llx pc=%08x icnt=%llu\n", (unsigned long long)pa, sz,
            (unsigned long long)v, (uint32_t)s->pc, (unsigned long long)s->icnt);
  }
  if(pa >= 0x80000000ULL and pa < 0x80001000ULL and sz >= 4) {
    uint32_t o = (uint32_t)(pa - 0x80000000ULL);
    r4030_write32(o, (uint32_t)v);
    if(sz == 8) {
      r4030_write32(o + 4, (uint32_t)(v >> 32));
    }
    return;
  }
  if(pa >= 0x80001000ULL and pa < 0x80001100ULL and sz >= 2) {
    sonic->write((uint32_t)(pa - 0x80001000ULL), (uint16_t)v);
    update_isr();
    return;
  }
  for(int i = 0; i < sz; i++) {
    write8(pa + i, (v >> (8*i)) & 0xff);
  }
  if(pa >= 0x60080000ULL and pa < 0x60082000ULL and sz >= 4) {
    uint32_t o = (uint32_t)(pa - 0x60080000ULL);
    uint32_t w = (uint32_t)v;
    if(o >= 0x800 and o < 0x1000) {
      int idx = (o - 0x800) >> 3;
      palette[idx][0] = (w >> 16) & 0xff;
      palette[idx][1] = (w >> 8) & 0xff;
      palette[idx][2] = w & 0xff;
    }
    else if(o == 0x118) {
      g364_width = w * 4;
    }
    else if(o == 0x150) {
      g364_height = w / 2;
    }
    else if(o == 0x300) {
      g364_ctla = w;
    }
    else if(o == 0x400) {
      g364_top = w;
    }
  }
}

void jazz::r4030_put32(uint32_t o, uint32_t v) {
  for(int i = 0; i < 4; i++) {
    r4030[(o + i) & 0xfff] = (v >> (8*i)) & 0xff;
  }
}

void jazz::r4030_write32(uint32_t o, uint32_t v) {
  switch(o)
    {
    case 0x058:                                  /* I/O cache byte mask: writes accumulate */
      r4030_put32(o, r4030_read32(o) | v);
      break;
    case 0x060:                                  /* I/O cache buffer window */
      /* with logical tag 0x80000001 and byte mask 0x0f0f0f0f the word lands in memory at
       * (physical tag & ~1) + 8 * (cache maintenance & 3), as QEMU's rc4030 does */
      if(r4030_read32(0x050) == 0x80000001u and r4030_read32(0x058) == 0x0f0f0f0fu) {
        uint64_t dest = (r4030_read32(0x048) & ~1u) + ((r4030_read32(0x030) & 3u) << 3);
        s->mem.set<uint32_t>(dest, v);
      }
      r4030_put32(o, v);
      break;
    case 0x228:                                  /* TIMER_INTERVAL: (v+1) ms from a 1 kHz clock */
      r4030_put32(o, v);
      timer_period = (uint64_t(v & 0x1ff) + 1) * ms_insns;
      timer_armed = true;
      next_tick = icnt + timer_period;
      break;
    default:
      r4030_put32(o, v);
      break;
    }
}

/* rc4030 IOMMU: move len bytes between a logical DMA address and real memory, translating each
 * 4 KiB page through the table at 0x18(base)/0x20(limit bytes); entry i = {frame, owner} LE32.
 * The SONIC drives the rc4030 purely as an IOMMU (no DMA channel), so it calls this directly. */
void jazz::iommu_rw(uint32_t logical, uint8_t *buf, uint32_t len, bool to_mem) {
  uint32_t tl_base = r4030_read32(0x18) & 0x7fffffffu, tl_entries = r4030_read32(0x20) / 8;
  uint32_t done = 0;
  while(done < len) {
    uint32_t la = logical + done;
    uint32_t n = std::min(len - done, 0x1000u - (la & 0xfffu));
    uint32_t page = la >> 12;
    if(page < tl_entries) {
      uint32_t frame = *reinterpret_cast<uint32_t*>(s->mem.get_raw_ptr(tl_base + 8 * page)) & ~0xfffu;
      uint8_t *p = s->mem.get_raw_ptr(frame | (la & 0xfffu));
      if(to_mem) {
        memcpy(p, buf + done, n);
      }
      else {
        memcpy(buf + done, p, n);
      }
    }
    else {
      fprintf(stderr, "[jazz] iommu: logical %08x beyond the translation table (%u entries)\n",
              la, tl_entries);
      memset(buf + done, 0, to_mem ? 0 : n);
    }
    done += n;
  }
}

void jazz::dma(int ch, uint8_t *buf, uint32_t len, bool to_mem) {
  uint32_t base = 0x100 + 0x20 * ch;
  uint32_t en = r4030_read32(base + 0x08) & ~0x0700u;    /* clear TC / MEM / ADDR interrupt flags */
  bool dev_to_mem = (en & 0x0002) == 0;
  if(not(en & 0x0001) or to_mem != dev_to_mem) {
    fprintf(stderr, "[jazz] dma ch%d: %s while enable=%08x icnt=%lu\n", ch, to_mem ? "write" : "read",
            en, (unsigned long)icnt);
    r4030_put32(base + 0x08, en | 0x0200);
    r4030_put32(0x200, r4030_read32(0x200) | (1u << ch));
    return;
  }
  len = std::min(len, r4030_read32(base + 0x10));
  iommu_rw(r4030_read32(base + 0x18), buf, len, to_mem);
  r4030_put32(base + 0x08, en | 0x0100);
  r4030_put32(base + 0x10, r4030_read32(base + 0x10) - len);
}

uint32_t jazz::r4030_read32(uint32_t o) const {
  if(o == 0x238) {
    return 7;                                    /* EISA interrupt acknowledge: nothing pending -> the i8259 spurious IRQ 7 */
  }
  uint32_t v = 0;
  for(int i = 0; i < 4; i++) {
    v |= uint32_t(r4030[(o + i) & 0xfff]) << (8*i);
  }
  return v;
}

uint8_t jazz::read8(uint64_t pa) {
  if(pa >= 0x08000000ULL and pa < 0x10000000ULL) {
    return 0;                                    /* no memory above 128 MiB */
  }
  if(pa >= 0x80000000ULL and pa < 0x80001000ULL) {
    return r4030[pa - 0x80000000ULL];
  }
  if(pa >= 0x80002000ULL and pa < 0x80002010ULL) {
    uint8_t v = esp->read((uint32_t)(pa - 0x80002000ULL));
    update_isr();
    return v;
  }
  if(pa >= 0x80003000ULL and pa < 0x80003008ULL) {
    return fdc_read((uint32_t)(pa - 0x80003000ULL));
  }
  if(pa >= 0x80004000ULL and pa < 0x80005000ULL) {
    return rtc_read(rtc_index);
  }
  if(pa >= 0x80005000ULL and pa < 0x80005002ULL) {
    return kbd_read((uint32_t)(pa - 0x80005000ULL));
  }
  if(pa >= 0x80006000ULL and pa < 0x80006008ULL) {
    return uart_read(0, (uint32_t)(pa - 0x80006000ULL));
  }
  if(pa >= 0x80007000ULL and pa < 0x80007008ULL) {
    return uart_read(1, (uint32_t)(pa - 0x80007000ULL));
  }
  if(pa >= 0x80008000ULL and pa < 0x80008008ULL) {
    uint32_t o = (uint32_t)(pa - 0x80008000ULL);   /* parallel port: no printer attached */
    return (o == 1) ? 0xd9 : (o == 2) ? lpt_ctrl : (o == 0) ? lpt_data : 0;
  }
  if(pa >= 0x80009000ULL and pa < 0x8000b000ULL) {
    return nvram[pa - 0x80009000ULL];
  }
  if(pa >= 0x8000b000ULL and pa < 0x8000b008ULL) {
    /* SONIC MAC PROM: 52:54:00:12:34:56, byte 7 = 0xff - end-around-carry byte sum */
    static const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint32_t o = (uint32_t)(pa - 0x8000b000ULL);
    if(o < 6) {
      return mac[o];
    }
    if(o == 7) {
      uint32_t c = 0;
      for(int i = 0; i < 6; i++) {
        c += mac[i];
        if(c > 0xff) {
          c = (c + 1) & 0xff;
        }
      }
      return 0xff - c;
    }
    return 0;
  }
  if(pa >= 0x8000d000ULL and pa < 0x8000e000ULL) {
    return 0xff;                                 /* DMA dummy */
  }
  if(pa >= 0x8000f000ULL and pa < 0x80010000ULL) {
    return led;
  }
  if(pa >= 0x60000000ULL and pa < 0x60080000ULL) {
    return (pa == 0x60000000ULL) ? 0x10 : 0;    /* G364 board ID ("Mips G364") */
  }
  if(pa >= 0x60080000ULL and pa < 0x60200000ULL) {
    return 0;                                    /* G364 control: write-only here (QEMU reads 0) */
  }
  if(pa >= 0x90000000ULL and pa < 0x90010000ULL) {
    if((pa & 0xffff) == 0x71) {
      return rtc_read(rtc_index);
    }
    return 0;
  }
  if(pa >= 0xf0000000ULL and pa < 0xf0001000ULL) {
    uint32_t o = (uint32_t)(pa - 0xf0000000ULL);
    if(o == 0) {
      uint16_t p = isr & imr;
      for(int i = 0; i < 16; i++) {
        if((p >> i) & 1) {
          return (i + 1) << 2;
        }
      }
      return 0;
    }
    if(o == 2) {
      return imr & 0xff;
    }
    if(o == 3) {
      return imr >> 8;
    }
    return 0;
  }
  log_unhandled("read", pa, 0, 1);
  return 0;
}

void jazz::write8(uint64_t pa, uint8_t v) {
  if(pa >= 0x08000000ULL and pa < 0x10000000ULL) {
    return;
  }
  if(pa >= 0x80000000ULL and pa < 0x80001000ULL) {
    r4030[pa - 0x80000000ULL] = v;
    return;
  }
  if(pa >= 0x80004000ULL and pa < 0x80005000ULL) {
    if(rtc_index < 128 and rtc_index >= 0xa) {
      cmos[rtc_index] = v;                       /* control/CMOS; time-of-day writes are ignored */
    }
    return;
  }
  if(pa >= 0x80002000ULL and pa < 0x80002010ULL) {
    esp->write((uint32_t)(pa - 0x80002000ULL), v);
    update_isr();
    return;
  }
  if(pa >= 0x80003000ULL and pa < 0x80003008ULL) {
    fdc_write((uint32_t)(pa - 0x80003000ULL), v);
    return;
  }
  if(pa >= 0x80005000ULL and pa < 0x80005002ULL) {
    kbd_write((uint32_t)(pa - 0x80005000ULL), v);
    return;
  }
  if(pa >= 0x80006000ULL and pa < 0x80006008ULL) {
    uart_write(0, (uint32_t)(pa - 0x80006000ULL), v);
    return;
  }
  if(pa >= 0x80007000ULL and pa < 0x80007008ULL) {
    uart_write(1, (uint32_t)(pa - 0x80007000ULL), v);
    return;
  }
  if(pa >= 0x80008000ULL and pa < 0x80008008ULL) {
    uint32_t o = (uint32_t)(pa - 0x80008000ULL);
    if(o == 0) {
      lpt_data = v;
    }
    else if(o == 2) {
      lpt_ctrl = v;
    }
    return;
  }
  if(pa >= 0x80009000ULL and pa < 0x8000b000ULL) {
    nvram[pa - 0x80009000ULL] = v;
    return;
  }
  if(pa >= 0x8000f000ULL and pa < 0x80010000ULL) {
    if(v != led) {
      fprintf(stderr, "[jazz] LED %02x icnt=%llu\n", v, (unsigned long long)icnt);
    }
    led = v;
    return;
  }
  if(pa >= 0x60000000ULL and pa < 0x60200000ULL) {
    return;                                      /* G364 ID ROM (ignored) / control (decoded in write()) */
  }
  if(pa >= 0x90000000ULL and pa < 0x90010000ULL) {
    if((pa & 0xffff) == 0x70) {
      rtc_index = v & 0x7f;
    }
    return;                                      /* i8259 / i8253 / DMA: programming only */
  }
  if(pa >= 0xf0000000ULL and pa < 0xf0001000ULL) {
    uint32_t o = (uint32_t)(pa - 0xf0000000ULL);
    if(o == 2) {
      imr = (imr & 0xff00) | v;
    }
    else if(o == 3) {
      imr = (imr & 0x00ff) | (uint16_t(v) << 8);
    }
    return;
  }
  if(pa >= 0x1fc00000ULL and pa < 0x20000000ULL) {
    return;
  }
  log_unhandled("write", pa, v, 1);
}

uint8_t jazz::rtc_read(uint8_t idx) {
  if(idx < 10 and idx != 1 and idx != 3 and idx != 5) {
    time_t t = rtc_base_sec + icnt / (1000 * ms_insns);
    struct tm tm;
    gmtime_r(&t, &tm);
    switch(idx)
      {
      case 0: return tm.tm_sec;
      case 2: return tm.tm_min;
      case 4: return tm.tm_hour;
      case 6: return tm.tm_wday + 1;
      case 7: return tm.tm_mday;
      case 8: return tm.tm_mon + 1;
      case 9: return (tm.tm_year + 1900) - 1980;  /* Jazz convention: years since 1980 */
      default: return 0;
      }
  }
  return cmos[idx & 0x7f];
}

void jazz::tick(uint64_t ic) {
  icnt = ic;
  if(timer_armed and icnt >= next_tick) {
    timer_pending = true;
    next_tick += timer_period;
  }
  sonic->tick(icnt);
  if(sonic->irq()) {
    update_isr();
  }
  while(not key_script.empty() and icnt >= key_script.front().icnt) {
    run_key_event(key_script.front());
    key_script.pop_front();
  }
  /* a keyboard sends one byte at a time, ~1 ms apart, only into an empty output buffer and only
   * while both the controller's keyboard interface (command byte bit 4) and scanning are enabled */
  if(not host_keys.empty() and kbd_out.empty() and kbd_scan and not(kbd_cmd_byte & 0x10) and
     icnt >= next_key) {
    kbd_out.push_back(host_keys.front());
    host_keys.pop_front();
    next_key = icnt + ms_insns;
    update_isr();
  }
}

void jazz::update_isr() {
  uint16_t p = 0;
  for(int u = 0; u < 2; u++) {
    if((com[u].ier & 0x02) and com[u].thre_pending) {
      p |= 1u << (u == 0 ? SRC_COM1 : SRC_COM2);
    }
  }
  if(fdc_irq) {
    p |= 1u << SRC_FDC;
  }
  if(esp->irq()) {
    p |= 1u << SRC_SCSI;
  }
  if(sonic->irq()) {
    p |= 1u << SRC_SONIC;
  }
  if(not kbd_out.empty()) {
    bool aux = (kbd_out.front() & 0x100) != 0;
    if(aux and (kbd_cmd_byte & 0x02)) {
      p |= 1u << SRC_MOUSE;
    }
    if(not aux and (kbd_cmd_byte & 0x01)) {
      p |= 1u << SRC_KBD;
    }
  }
  isr = p;
}

bool jazz::ip3() const {
  return (isr & imr) != 0;
}

/* ---- 16550 ---- */
uint8_t jazz::uart_read(int u, uint32_t o) {
  uart &c = com[u];
  bool dlab = (c.lcr & 0x80) != 0;
  uint8_t r = 0;
  switch(o)
    {
    case 0:
      if(dlab) {
        r = c.dll;
      }
      else if(not c.rx.empty()) {
        r = c.rx.front();
        c.rx.pop_front();
      }
      break;
    case 1:
      r = dlab ? c.dlm : c.ier;
      break;
    case 2:
      r = 0x01;
      if((c.ier & 0x02) and c.thre_pending) {
        r = 0x02;
        c.thre_pending = false;
      }
      r |= c.fifo_en ? 0xc0 : 0x00;
      break;
    case 3:
      r = c.lcr;
      break;
    case 4:
      r = c.mcr;
      break;
    case 5:
      r = 0x60 | (c.rx.empty() ? 0x00 : 0x01);   /* THRE | TEMT, DR when the receive FIFO has data */
      break;
    case 6:
      if(c.mcr & 0x10) {
        r = ((c.mcr & 0x02) ? 0x10 : 0) | ((c.mcr & 0x01) ? 0x20 : 0) |
          ((c.mcr & 0x04) ? 0x40 : 0) | ((c.mcr & 0x08) ? 0x80 : 0);
      }
      else {
        r = 0xb0;
      }
      break;
    case 7:
      r = c.scr;
      break;
    default:
      break;
    }
  update_isr();
  return r;
}

void jazz::uart_write(int u, uint32_t o, uint8_t v) {
  uart &c = com[u];
  bool dlab = (c.lcr & 0x80) != 0;
  switch(o)
    {
    case 0:
      if(dlab) {
        c.dll = v;
      }
      else {
        if(c.mcr & 0x10) {
          c.rx.push_back(v);                       /* loopback: TSR -> RSR (the ROM's serial POST) */
        }
        else {
          fputc(v, u == 0 ? stdout : stderr);
          fflush(u == 0 ? stdout : stderr);
        }
        c.thre_pending = true;
      }
      break;
    case 1:
      if(dlab) {
        c.dlm = v;
      }
      else {
        if((v & 0x02) and not(c.ier & 0x02)) {
          c.thre_pending = true;
        }
        c.ier = v & 0x0f;
      }
      break;
    case 2:
      c.fifo_en = (v & 0x01) != 0;
      break;
    case 3:
      c.lcr = v;
      break;
    case 4:
      c.mcr = v;
      break;
    case 7:
      c.scr = v;
      break;
    default:
      break;
    }
  update_isr();
}

/* ---- 82077 floppy controller with no drives: enough for the ROM's DOR read-back POST and its
 * init (reset -> IRQ -> 4x SENSE INTERRUPT, CONFIGURE, SPECIFY), per the J0 QEMU trace ---- */
void jazz::fdc_reset() {
  fdc_cmd = 0;
  fdc_params = 0;
  fdc_result.clear();
  fdc_sense = 4;                                 /* one SENSE INTERRUPT result per drive */
  fdc_irq = true;
}

uint8_t jazz::fdc_read(uint32_t o) {
  uint8_t r = 0;
  switch(o)
    {
    case 2:
      r = fdc_dor;
      break;
    case 4:                                      /* MSR: RQM, plus DIO|CB for results, CB for params */
      r = not fdc_result.empty() ? 0xd0 : (fdc_params != 0) ? 0x90 : 0x80;
      break;
    case 5:
      if(not fdc_result.empty()) {
        r = fdc_result.front();
        fdc_result.pop_front();
      }
      break;
    default:
      break;
    }
  return r;
}

void jazz::fdc_write(uint32_t o, uint8_t v) {
  switch(o)
    {
    case 2:
      if((v & 0x04) and not(fdc_dor & 0x04)) {   /* leaving reset */
        fdc_reset();
      }
      fdc_dor = v;
      break;
    case 4:
      if(v & 0x80) {                             /* DSR software reset */
        fdc_reset();
      }
      break;
    case 5:
      if(fdc_params != 0) {                      /* parameter byte; CONFIGURE/SPECIFY have no results */
        fdc_params--;
        break;
      }
      fdc_cmd = v;
      if(v == 0x08) {                            /* SENSE INTERRUPT: ST0, PCN */
        fdc_irq = false;
        if(fdc_sense != 0) {
          fdc_result.push_back(0xc0 | (4 - fdc_sense));
          fdc_result.push_back(0);
          fdc_sense--;
        }
        else {
          fdc_result.push_back(0x80);
        }
      }
      else if(v == 0x13) {                       /* CONFIGURE */
        fdc_params = 3;
      }
      else if(v == 0x03) {                       /* SPECIFY */
        fdc_params = 2;
      }
      else {
        fprintf(stderr, "[jazz] fdc command %02x (invalid) icnt=%lu\n", v, (unsigned long)icnt);
        fdc_result.push_back(0x80);
      }
      break;
    default:
      break;
    }
  update_isr();
}

/* ---- i8042 keyboard/mouse controller (no host input yet) ---- */
uint8_t jazz::kbd_read(uint32_t o) {
  if(o == 1) {                                   /* status */
    uint8_t st = kbd_selftest ? 0x04 : 0x00;     /* SYS */
    if(not kbd_out.empty()) {
      st |= 0x01;                                /* OBF */
      if(kbd_out.front() & 0x100) {
        st |= 0x20;                              /* AUX OBF */
      }
    }
    return st;
  }
  uint8_t d = 0;
  if(not kbd_out.empty()) {
    d = kbd_out.front() & 0xff;
    kbd_out.pop_front();
  }
  update_isr();
  return d;
}

void jazz::kbd_write(uint32_t o, uint8_t v) {
  if(o == 1) {                                   /* controller command */
    switch(v)
      {
      case 0xaa: kbd_selftest = true; kbd_out.push_back(0x55); break;
      case 0xab: kbd_out.push_back(0x00); break;   /* keyboard interface test */
      case 0xa9: kbd_out.push_back(0x00); break;   /* aux interface test */
      case 0x20: kbd_out.push_back(kbd_cmd_byte); break;
      case 0x60: case 0xd1: case 0xd2: case 0xd3: case 0xd4: kbd_pending_cmd = v; break;
      case 0xa7: kbd_cmd_byte |= 0x20; break;
      case 0xa8: kbd_cmd_byte &= ~0x20; break;
      case 0xad: kbd_cmd_byte |= 0x10; break;
      case 0xae: kbd_cmd_byte &= ~0x10; break;
      case 0xfe: reset_req = true; break;
      default: break;
      }
    update_isr();
    return;
  }
  uint8_t pc = kbd_pending_cmd;                  /* data port */
  kbd_pending_cmd = 0;
  switch(pc)
    {
    case 0x60: kbd_cmd_byte = v; break;
    case 0xd1: if(not(v & 0x01)) { reset_req = true; } break;  /* output port: bit 0 = reset */
    case 0xd2: kbd_out.push_back(v); break;
    case 0xd3: kbd_out.push_back(0x100 | v); break;
    case 0xd4: kbd_data_to_device(v, true); break;
    default: kbd_data_to_device(v, false); break;
    }
  update_isr();
}

void jazz::kbd_data_to_device(uint8_t v, bool aux) {
  uint16_t a = aux ? 0x100 : 0;
  if(aux and aux_dev_arg != 0) {                /* argument of F3 (sample rate) / E8 (resolution) */
    aux_dev_arg = 0;
    kbd_out.push_back(a | 0xfa);
    return;
  }
  if(aux) {
    switch(v)
      {
      case 0xf3:                                /* set sample rate: a byte follows */
      case 0xe8:                                /* set resolution: a byte follows */
        aux_dev_arg = v;
        break;
      case 0xf4:                                /* enable data reporting -> movement packets flow */
        mouse_stream = true;
        break;
      case 0xf5:                                /* disable data reporting */
      case 0xff:                                /* reset also stops reporting */
        mouse_stream = false;
        break;
      default:
        break;
      }
  }
  if(not aux and kbd_dev_arg != 0) {            /* argument of ED (LEDs) / F0 (scan set) / F3 (rate) */
    kbd_dev_arg = 0;
    kbd_out.push_back(0xfa);
    return;
  }
  if(not aux) {
    switch(v)
      {
      case 0xed:
      case 0xf0:
      case 0xf3:
        kbd_dev_arg = v;
        break;
      case 0xf4:
      case 0xff:
        kbd_scan = true;
        break;
      case 0xf5:
        kbd_scan = false;
        break;
      default:
        break;
      }
  }
  switch(v)
    {
    case 0xff:                                   /* reset: ACK, self-test passed (+ mouse ID) */
      kbd_out.push_back(a | 0xfa);
      kbd_out.push_back(a | 0xaa);
      if(aux) {
        kbd_out.push_back(a | 0x00);
      }
      break;
    case 0xf2:                                   /* identify */
      kbd_out.push_back(a | 0xfa);
      if(aux) {
        kbd_out.push_back(a | 0x00);
      }
      else {
        kbd_out.push_back(0xab);
        kbd_out.push_back(0x83);
      }
      break;
    case 0xee:                                   /* echo */
      kbd_out.push_back(a | 0xee);
      break;
    default:                                     /* everything else (and its argument byte): ACK */
      kbd_out.push_back(a | 0xfa);
      break;
    }
}

/* ---- --keys: host keyboard input ---- */
/* {name, set-1 code, set-2 code, E0-extended}; the controller emits set 1 when the command byte
 * asks it to translate (bit 6, what NT uses), else the keyboard's native set 2 */
struct key_def {
  const char *name;
  uint8_t s1, s2;
  bool ext;
};
static const key_def g_keys[] = {
  {"a", 0x1e, 0x1c, false}, {"b", 0x30, 0x32, false}, {"c", 0x2e, 0x21, false}, {"d", 0x20, 0x23, false},
  {"e", 0x12, 0x24, false}, {"f", 0x21, 0x2b, false}, {"g", 0x22, 0x34, false}, {"h", 0x23, 0x33, false},
  {"i", 0x17, 0x43, false}, {"j", 0x24, 0x3b, false}, {"k", 0x25, 0x42, false}, {"l", 0x26, 0x4b, false},
  {"m", 0x32, 0x3a, false}, {"n", 0x31, 0x31, false}, {"o", 0x18, 0x44, false}, {"p", 0x19, 0x4d, false},
  {"q", 0x10, 0x15, false}, {"r", 0x13, 0x2d, false}, {"s", 0x1f, 0x1b, false}, {"t", 0x14, 0x2c, false},
  {"u", 0x16, 0x3c, false}, {"v", 0x2f, 0x2a, false}, {"w", 0x11, 0x1d, false}, {"x", 0x2d, 0x22, false},
  {"y", 0x15, 0x35, false}, {"z", 0x2c, 0x1a, false},
  {"1", 0x02, 0x16, false}, {"2", 0x03, 0x1e, false}, {"3", 0x04, 0x26, false}, {"4", 0x05, 0x25, false},
  {"5", 0x06, 0x2e, false}, {"6", 0x07, 0x36, false}, {"7", 0x08, 0x3d, false}, {"8", 0x09, 0x3e, false},
  {"9", 0x0a, 0x46, false}, {"0", 0x0b, 0x45, false},
  {"-", 0x0c, 0x4e, false}, {"=", 0x0d, 0x55, false}, {"[", 0x1a, 0x54, false}, {"]", 0x1b, 0x5b, false},
  {";", 0x27, 0x4c, false}, {"'", 0x28, 0x52, false}, {"`", 0x29, 0x0e, false}, {"\\", 0x2b, 0x5d, false},
  {",", 0x33, 0x41, false}, {".", 0x34, 0x49, false}, {"/", 0x35, 0x4a, false},
  {"esc", 0x01, 0x76, false}, {"backspace", 0x0e, 0x66, false}, {"tab", 0x0f, 0x0d, false},
  {"enter", 0x1c, 0x5a, false}, {"ctrl", 0x1d, 0x14, false}, {"shift", 0x2a, 0x12, false},
  {"rshift", 0x36, 0x59, false}, {"alt", 0x38, 0x11, false}, {"space", 0x39, 0x29, false},
  {"capslock", 0x3a, 0x58, false},
  {"f1", 0x3b, 0x05, false}, {"f2", 0x3c, 0x06, false}, {"f3", 0x3d, 0x04, false}, {"f4", 0x3e, 0x0c, false},
  {"f5", 0x3f, 0x03, false}, {"f6", 0x40, 0x0b, false}, {"f7", 0x41, 0x83, false}, {"f8", 0x42, 0x0a, false},
  {"f9", 0x43, 0x01, false}, {"f10", 0x44, 0x09, false}, {"f11", 0x57, 0x78, false}, {"f12", 0x58, 0x07, false},
  {"insert", 0x52, 0x70, true}, {"delete", 0x53, 0x71, true}, {"home", 0x47, 0x6c, true},
  {"end", 0x4f, 0x69, true}, {"pgup", 0x49, 0x7d, true}, {"pgdn", 0x51, 0x7a, true},
  {"up", 0x48, 0x75, true}, {"down", 0x50, 0x72, true}, {"left", 0x4b, 0x6b, true}, {"right", 0x4d, 0x74, true},
};
static const int N_KEYS = sizeof(g_keys) / sizeof(g_keys[0]);
static const char *g_shifted = "!@#$%^&*()_+{}:\"~|<>?";
static const char *g_unshifted = "1234567890-=[];'`\\,./";

static int find_key(const std::string &name) {
  for(int i = 0; i < N_KEYS; i++) {
    if(name == g_keys[i].name) {
      return i;
    }
  }
  return -1;
}

void jazz::key_bytes(int k, bool make) {
  const key_def &d = g_keys[k];
  if(d.ext) {
    host_keys.push_back(0xe0);
  }
  if(kbd_cmd_byte & 0x40) {
    host_keys.push_back(make ? d.s1 : (d.s1 | 0x80));
  }
  else {
    if(not make) {
      host_keys.push_back(0xf0);
    }
    host_keys.push_back(d.s2);
  }
}

void jazz::run_key_event(const key_event &e) {
  fprintf(stderr, "[jazz] keys @%lu: %s %s\n", (unsigned long)icnt, e.op.c_str(), e.arg.c_str());
  if(e.op == "screen") {
    dump_screen(e.arg.c_str());
    return;
  }
  if(e.op == "key") {                            /* a+b+c: press in order, release in reverse */
    std::vector<int> ks;
    size_t p = 0;
    while(p <= e.arg.size()) {
      size_t q = e.arg.find('+', p);
      if(q == std::string::npos) {
        q = e.arg.size();
      }
      int k = find_key(e.arg.substr(p, q - p));
      if(k < 0) {
        fprintf(stderr, "[jazz] keys: unknown key '%s'\n", e.arg.substr(p, q - p).c_str());
        return;
      }
      ks.push_back(k);
      p = q + 1;
    }
    for(int k : ks) {
      key_bytes(k, true);
    }
    for(auto it = ks.rbegin(); it != ks.rend(); ++it) {
      key_bytes(*it, false);
    }
    return;
  }
  if(e.op == "type") {
    for(char c : e.arg) {
      bool shift = false;
      std::string name;
      if(c >= 'A' and c <= 'Z') {
        shift = true;
        name = std::string(1, c - 'A' + 'a');
      }
      else if(c == ' ') {
        name = "space";
      }
      else if(const char *sp = strchr(g_shifted, c)) {
        shift = true;
        name = std::string(1, g_unshifted[sp - g_shifted]);
      }
      else {
        name = std::string(1, c);
      }
      int k = find_key(name);
      if(k < 0) {
        fprintf(stderr, "[jazz] keys: can't type '%c'\n", c);
        continue;
      }
      if(shift) {
        key_bytes(find_key("shift"), true);
      }
      key_bytes(k, true);
      key_bytes(k, false);
      if(shift) {
        key_bytes(find_key("shift"), false);
      }
    }
    return;
  }
  fprintf(stderr, "[jazz] keys: unknown op '%s'\n", e.op.c_str());
}

bool jazz::load_keys(const std::string &fn) {
  FILE *f = fopen(fn.c_str(), "r");
  if(f == nullptr) {
    return false;
  }
  char line[1024];
  while(fgets(line, sizeof(line), f)) {
    std::string l(line);
    while(not l.empty() and (l.back() == '\n' or l.back() == '\r')) {
      l.pop_back();
    }
    size_t a = l.find_first_not_of(" \t");
    if(a == std::string::npos or l[a] == '#') {
      continue;
    }
    key_event e;
    char *end = nullptr;
    e.icnt = strtoull(l.c_str() + a, &end, 0);
    std::string rest(end);
    size_t b = rest.find_first_not_of(" \t");
    size_t c = rest.find_first_of(" \t", b);
    e.op = rest.substr(b, c == std::string::npos ? std::string::npos : c - b);
    size_t d = (c == std::string::npos) ? std::string::npos : rest.find_first_not_of(" \t", c);
    if(d != std::string::npos) {
      e.arg = rest.substr(d);                    /* the rest of the line: `type` keeps inner spaces */
    }
    key_script.push_back(e);
  }
  fclose(f);
  return true;
}

/* ---- G364 frame buffer dump ---- */
void jazz::dump_screen(const char *fn) {
  uint32_t w = g364_width ? g364_width : 1280, h = g364_height ? g364_height : 1024;
  FILE *f = fopen(fn, "wb");
  if(f == nullptr) {
    return;
  }
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  const uint8_t *vram = s->mem.get_raw_ptr(0x40000000ULL + (g364_top & 0x7fffff));
  for(uint32_t i = 0; i < w * h; i++) {
    fwrite(palette[vram[i]], 1, 3, f);
  }
  fclose(f);
}

/* ---- --sdl: live framebuffer export + host input injection ---- */
uint32_t jazz::fb_width()  { return g364_width  ? g364_width  : 1280; }
uint32_t jazz::fb_height() { return g364_height ? g364_height : 1024; }

void jazz::fb_rgba(uint32_t *dst) {
  uint32_t w = fb_width(), h = fb_height();
  const uint8_t *vram = s->mem.get_raw_ptr(0x40000000ULL + (g364_top & 0x7fffff));
  for(uint32_t i = 0; i < w * h; i++) {
    const uint8_t *p = palette[vram[i]];
    dst[i] = 0xff000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
  }
}

void jazz::inject_key_name(const char *name, bool make) {
  int k = find_key(name);
  if(k >= 0) {                                   /* queued in host_keys; tick() paces them into the i8042 */
    key_bytes(k, make);
  }
}

void jazz::inject_mouse(int dx, int dy, uint8_t buttons) {
  if(not mouse_stream or kbd_out.size() > 64) {
    return;                                      /* not streaming, or the guest isn't draining */
  }
  if(dx >  127) { dx =  127; }
  if(dx < -128) { dx = -128; }
  int my = -dy;                                  /* PS/2 +Y is up; screen +Y is down */
  if(my >  127) { my =  127; }
  if(my < -128) { my = -128; }
  uint8_t b0 = 0x08 | (buttons & 0x07);          /* bit3 always 1 */
  if(dx < 0) { b0 |= 0x10; }                     /* X sign */
  if(my < 0) { b0 |= 0x20; }                     /* Y sign */
  kbd_out.push_back(0x100 | b0);                 /* 3-byte standard PS/2 packet on the aux port */
  kbd_out.push_back(0x100 | (uint8_t)dx);
  kbd_out.push_back(0x100 | (uint8_t)my);
  update_isr();
}
