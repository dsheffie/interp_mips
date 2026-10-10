#ifndef __JAZZ_HH__
#define __JAZZ_HH__

#include <cstdint>
#include <cstdio>
#include <deque>
#include <set>
#include <string>

struct state_t;
class jazz_esp;
class jazz_sonic;

/* jazz: a Microsoft Jazz / MIPS Magnum 4000 platform for interp_mips (--jazz), little-endian.
 * Device behaviour follows the J0 QEMU traces and register-level docs (QEMU hw/mips/jazz.c,
 * hw/dma/rc4030.c, hw/display/g364fb.c, Linux asm/jazz.h); see ~/code/jazz/JAZZ_NT_HANDOFF.md.
 *
 * Physical map (anything >= 0x08000000 is routed here; RAM is 0..128 MiB):
 *   0x08000000..0x0fffffff  no memory (reads 0, writes dropped) so firmware memory sizing sees 128 MiB
 *   0x1fc00000 / 0xfff00000 the PROM image (two aliases; writes ignored)
 *   0x40000000              G364 VRAM (plain memory)
 *   0x60000000 / 0x60080000 G364 ID byte / control registers (palette, timing, top of screen)
 *   0x80000000              R4030: storage with QEMU reset values + interval timer (IP6)
 *   0x80001000 .. 0x8000f000 local devices: SONIC (absent), ESP (jazz_esp), floppy, RTC data, i8042,
 *                           COM1/COM2, parallel, NVRAM (file-backed), MAC PROM, dummy DMA, LED
 *   0x90000000              ISA I/O (RTC index at 0x70, otherwise storage)
 *   0xf0000000              local interrupt source / enable (IP3)
 * Time base: instructions retired; one R4030 "millisecond" is ms_insns instructions. */
class jazz {
public:
  jazz(state_t *s, const std::string &nvram_file, uint64_t ms_insns, const std::string &disk,
       bool disk_write, const std::string &net_tap);
  ~jazz();
  bool is_memory(uint64_t pa) const;           /* RAM-like (VRAM, PROM): served from sparse_mem */
  uint64_t read(uint64_t pa, int sz);
  void write(uint64_t pa, uint64_t v, int sz);
  void tick(uint64_t icnt);                    /* every instruction */
  bool ip3() const;                            /* local devices */
  bool ip6() const { return timer_pending; }   /* R4030 interval timer */
  bool reset_requested() const { return reset_req; }
  void dump_screen(const char *fn);            /* G364 frame buffer -> PPM */
  /* --keys: timed keyboard script, one event per line: <icnt> key <a+b+...> | type <text> |
   * screen <file.ppm>  ('#' comments).  Returns false if the file can't be read. */
  bool load_keys(const std::string &fn);
  /* R4030 DMA channel ch: move len bytes between buf and the channel's (translated) address */
  void dma(int ch, uint8_t *buf, uint32_t len, bool to_mem);
  /* rc4030 as a plain IOMMU: move len bytes to/from a translated logical DMA address (SONIC) */
  void iommu_rw(uint32_t logical, uint8_t *buf, uint32_t len, bool to_mem);
  /* --sdl live display + input (jazz_sdl.cc) */
  uint32_t fb_width();
  uint32_t fb_height();
  void fb_rgba(uint32_t *dst);                 /* w*h pixels, 0xFFRRGGBB from the G364 palette+VRAM */
  void inject_key_name(const char *name, bool make);   /* host key down(make)/up */
  void inject_mouse(int dx, int dy, uint8_t buttons);  /* host PS/2 mouse (aux port) */
private:
  state_t *s;
  uint64_t ms_insns, icnt = 0;
  uint64_t rtc_base_sec;                       /* host time at start */
  /* R4030 */
  uint8_t r4030[0x1000];
  bool timer_armed = false, timer_pending = false;
  uint64_t timer_period = 0, next_tick = 0;
  uint16_t imr = 0x10, isr = 0;                /* local interrupt enable / pending */
  jazz_esp *esp = nullptr;                     /* 53C94 SCSI at 0x80002000 */
  jazz_sonic *sonic = nullptr;                 /* DP83932 Ethernet at 0x80001000 */
  /* RTC (DS1287) */
  uint8_t rtc_index = 0, cmos[128];
  /* NVRAM (DS1225, 8 KiB) */
  uint8_t *nvram = nullptr;
  int nvram_fd = -1;
  /* G364 */
  uint8_t palette[256][3];
  uint32_t g364_width = 0, g364_height = 0, g364_top = 0, g364_ctla = 0;
  uint8_t cursor_palette[3][3] = {};
  uint16_t cursor_pat[512] = {};               /* 64x64 hardware cursor, 2 bits/pixel, 8 px per entry */
  uint32_t cursor_pos = 0xffffffffu;           /* x << 12 | y (top-left) */
  bool video_irq = false;                      /* G364 vertical retrace, cleared by any control write */
  uint64_t next_vblank = 0;
  /* i8042 */
  std::deque<uint16_t> kbd_out;                /* bit 8 = from the aux (mouse) port */
  uint8_t kbd_cmd_byte = 0x45, kbd_pending_cmd = 0;
  bool kbd_selftest = false;
  bool kbd_scan = true;                        /* keyboard scanning (device F4 enable / F5 disable) */
  bool mouse_stream = false;                   /* aux data reporting enabled (0xf4 / 0xf5) */
  uint8_t aux_dev_arg = 0;                     /* aux command awaiting its argument byte (F3/E8) */
  uint8_t aux_rate = 100, aux_res = 2;         /* sample rate / resolution, reported by E9 */
  bool aux_scale21 = false;                    /* E7 2:1 scaling / E6 1:1 */
  uint8_t kbd_dev_arg = 0;                     /* device command waiting for its argument byte */
  std::deque<uint8_t> host_keys;               /* scan codes from --keys, not yet in kbd_out */
  uint64_t next_key = 0;
  int mouse_dx = 0, mouse_dy = 0;              /* host motion not yet sent (coalesced into packets) */
  uint8_t mouse_btn = 0, mouse_sent_btn = 0;
  uint64_t next_mouse = 0;
  struct key_event {
    uint64_t icnt;
    std::string op, arg;
  };
  std::deque<key_event> key_script;
  /* 16550 x 2 */
  struct uart {
    uint8_t ier = 0, lcr = 0, mcr = 0, scr = 0, dll = 0, dlm = 0;
    bool fifo_en = false, thre_pending = false;
    std::deque<uint8_t> rx;                    /* receive FIFO (loopback only for now) */
  } com[2];
  /* 82077 floppy controller, no drives */
  uint8_t fdc_dor = 0, fdc_cmd = 0, fdc_params = 0, fdc_sense = 0;
  bool fdc_irq = false;
  std::deque<uint8_t> fdc_result;
  uint8_t led = 0xff, lpt_data = 0, lpt_ctrl = 0xc0;
  bool reset_req = false;
  std::set<uint64_t> logged;                   /* unhandled 16-byte blocks already reported */

  uint64_t read_dev(uint64_t pa, int sz);
  void log_unhandled(const char *what, uint64_t pa, uint64_t v, int sz);
  uint8_t read8(uint64_t pa);
  void write8(uint64_t pa, uint8_t v);
  uint8_t rtc_read(uint8_t idx);
  uint8_t uart_read(int u, uint32_t o);
  void uart_write(int u, uint32_t o, uint8_t v);
  uint8_t kbd_read(uint32_t o);
  void kbd_write(uint32_t o, uint8_t v);
  void kbd_data_to_device(uint8_t v, bool aux);
  void run_key_event(const key_event &e);
  void key_bytes(int k, bool make);
  uint8_t fdc_read(uint32_t o);
  void fdc_write(uint32_t o, uint8_t v);
  void fdc_reset();
  uint32_t r4030_read32(uint32_t o) const;
  void r4030_put32(uint32_t o, uint32_t v);
  void r4030_write32(uint32_t o, uint32_t v);
  void update_isr();
};

#endif
