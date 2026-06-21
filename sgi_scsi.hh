#ifndef __sgi_scsi__
#define __sgi_scsi__

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>

struct state_t;

/*
 * Fused WD33C93A SCSI controller + single disk target (behavioral model).
 *
 * On IP22 the CPU programs the WD33C93 through two byte ports in the HPC3 HD0
 * window: SASR (0x44003, the indirect register-select pointer) and SCMD
 * (0x44007, the data port for the selected register; auto-increments the
 * pointer).  The HPC3 SCSI-DMA channel moves the data phase byte-at-a-time via
 * dma_r()/dma_w().
 *
 * We collapse the SCSI bus + disk target into one model (no arbitration /
 * REQ-ACK microstates -- MAME needs those for a general bus, we have one disk).
 * Control is an explicit data-phase state machine (see phase_t): a
 * Select-And-Transfer (0x08/0x09) decodes the CDB, runs the data phase against
 * the disk image, and completes with WD33C93 SCSI Status 0x16 + INTRQ.  Command
 * set validated against a live IRIX 6.5 MAME boot trace.  Writes go to an
 * in-memory COW overlay so the backing image is never modified.
 */
class sgi_scsi {
public:
  /* Externally-visible control FSM (the data phase the HPC3 DMA interacts with). */
  enum phase_t { PH_IDLE, PH_DATA_IN, PH_DATA_OUT };
private:
  state_t *s;
  int      fd = -1;                 /* raw disk image (read-only) */
  uint64_t nblocks = 0;             /* 512-byte block count */
  uint8_t  regs[0x20] = {0};        /* WD33C93 register file 0x00..0x1f */
  uint8_t  sasr = 0;                /* indirect register pointer */
  phase_t  phase = PH_IDLE;         /* data-phase state */
  std::vector<uint8_t> buf;         /* current data-in / data-out buffer */
  size_t   pos = 0;                 /* DMA cursor into buf */
  bool     drq = false;             /* DRQ: data available (in) / needed (out) */
  bool     intrq = false;           /* INTRQ pending (command complete) */
  uint64_t wr_lba = 0;              /* pending WRITE target (drained from buf) */
  uint8_t  sense[18] = {0};         /* REQUEST SENSE fixed-format data */
  uint8_t  tgt_status = 0;          /* target STATUS byte -> WD33C93 reg 0x0f after a transfer */
  std::unordered_map<uint64_t, std::vector<uint8_t>> overlay; /* COW writes */

  void block_read(uint64_t lba, uint8_t *dst);     /* 512 bytes */
  void block_write(uint64_t lba, const uint8_t *src);
  void exec_command(uint8_t cc);    /* a WD33C93 command was written to reg 0x18 */
  void select_and_transfer();       /* decode CDB -> arm data phase / complete */
  void begin_data_in(const uint8_t *p, size_t n);  /* -> PH_DATA_IN */
  void begin_data_out(size_t n);                   /* -> PH_DATA_OUT */
  void finish();                    /* data phase drained -> status + INTRQ */
  void complete(uint8_t scsi_status);
public:
  sgi_scsi(state_t *s, const std::string &disk_path);
  ~sgi_scsi();
  bool ok() const { return fd >= 0; }

  /* CPU PIO to the WD33C93 indirect register file (port 0 = SASR, 1 = SCMD). */
  void    pio_w(uint32_t port, uint8_t v);
  uint8_t pio_r(uint32_t port);

  /* HPC3 SCSI-DMA channel data byte port. */
  uint8_t dma_r();                  /* device -> memory */
  void    dma_w(uint8_t v);         /* memory -> device */

  void reset();
  bool drq_pending()   const { return drq; }
  bool intrq_pending() const { return intrq; }
};

#endif
