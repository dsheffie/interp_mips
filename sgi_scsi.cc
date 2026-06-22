#include "sgi_scsi.hh"
#include "interpret.hh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static const bool scsi_verbose = getenv("SCSIDBG") != nullptr;
#define SPRINTF(...) do { if(scsi_verbose) fprintf(stderr, __VA_ARGS__); } while(0)

/* WD33C93 register file indices */
enum { WD_OWN_ID=0x00, WD_CONTROL=0x01, WD_CDB=0x03, WD_TARGET_LUN=0x0f,
       WD_COMMAND_PHASE=0x10, WD_XFER_CNT_MSB=0x12, WD_DEST_ID=0x15,
       WD_SOURCE_ID=0x16, WD_SCSI_STATUS=0x17, WD_COMMAND=0x18,
       WD_DATA=0x19, WD_AUX_STATUS=0x1f };
/* Auxiliary status bits */
enum { AUX_DBR=0x01, AUX_CIP=0x10, AUX_BSY=0x20, AUX_LCI=0x40, AUX_INT=0x80 };
/* WD33C93 commands (low 7 bits of reg 0x18) */
enum { CMD_RESET=0x00, CMD_SEL_ATN_XFER=0x08, CMD_SEL_XFER=0x09, CMD_XFER_INFO=0x20 };
/* SCSI Status completion codes (reg 0x17) */
enum { ST_RESET=0x00, ST_SELECT_TRANSFER_SUCCESS=0x16 };

sgi_scsi::sgi_scsi(state_t *s, const std::string &disk_path) : s(s) {
  fd = open(disk_path.c_str(), O_RDONLY);
  if(fd < 0) {
    fprintf(stderr, "sgi_scsi: cannot open disk image '%s'\n", disk_path.c_str());
    return;
  }
  struct stat st;
  if(fstat(fd, &st) == 0)
    nblocks = (uint64_t)st.st_size / 512;
  sense[0] = 0x70; sense[7] = 0x0a;   /* fixed-format REQUEST SENSE: NO SENSE */
  reset();
  SPRINTF("sgi_scsi: '%s' = %llu blocks (%llu MB)\n", disk_path.c_str(),
          (unsigned long long)nblocks, (unsigned long long)(nblocks/2048));
}

sgi_scsi::~sgi_scsi() { if(fd >= 0) close(fd); }

void sgi_scsi::reset() {
  sasr = 0; pos = 0; drq = false; intrq = false; phase = PH_IDLE;
  buf.clear();
  regs[WD_AUX_STATUS] = 0;
}

void sgi_scsi::block_read(uint64_t lba, uint8_t *dst) {
  auto it = overlay.find(lba);            /* COW: written blocks win */
  if(it != overlay.end()) { memcpy(dst, it->second.data(), 512); return; }
  if(lba >= nblocks) { memset(dst, 0, 512); return; }
  ssize_t n = pread(fd, dst, 512, (off_t)(lba * 512));
  if(n != 512) memset(dst, 0, 512);
}

void sgi_scsi::block_write(uint64_t lba, const uint8_t *src) {
  auto &v = overlay[lba];                 /* never touch the backing image */
  v.assign(src, src + 512);
}

/* ---- data-phase FSM transitions ------------------------------------------ */

void sgi_scsi::begin_data_in(const uint8_t *p, size_t n) {
  buf.assign(p, p + n);
  pos = 0;
  if(n == 0) { finish(); return; }
  phase = PH_DATA_IN; drq = true;
}

void sgi_scsi::begin_data_out(size_t n) {
  buf.assign(n, 0);
  pos = 0;
  if(n == 0) { finish(); return; }
  phase = PH_DATA_OUT; drq = true;
}

void sgi_scsi::finish() {
  /* drain side-effects for data-out, then post completion status + INTRQ */
  if(phase == PH_DATA_OUT && regs[WD_CDB] == 0x2a) {      /* WRITE(10) -> overlay */
    size_t blocks = buf.size() / 512;
    for(size_t i = 0; i < blocks; i++)
      block_write(wr_lba + i, buf.data() + i * 512);
  }
  phase = PH_IDLE; drq = false;
  complete(ST_SELECT_TRANSFER_SUCCESS);
}

void sgi_scsi::complete(uint8_t scsi_status) {
  regs[WD_SCSI_STATUS]   = scsi_status;
  /* After Select-and-Transfer the WD33C93 leaves the target STATUS byte in reg
   * 0x0f (Target LUN). IRIX reads it to tell GOOD (0x00) from CHECK CONDITION
   * (0x02). We must write it -- otherwise IRIX reads back the LUN it programmed
   * and mistakes lun>=2 for a CHECK CONDITION (an INQUIRY/REQUEST-SENSE loop). */
  regs[WD_TARGET_LUN]    = tgt_status;
  regs[WD_COMMAND_PHASE] = 0x60;          /* command complete */
  /* The WD33C93 decrements its 24-bit Transfer Count (regs 0x12-0x14) as bytes
   * move; at completion it has counted down to 0. We transfer exactly the
   * programmed amount, so post a zero residual -- otherwise the sgiwd93 driver
   * reads the leftover programmed count as a short transfer and sets b_error on
   * the buffer, which makes xfs_read_file bail (chunkread -> b_error -> ENOEXEC). */
  regs[0x12] = regs[0x13] = regs[0x14] = 0;
  regs[WD_AUX_STATUS]   &= ~(AUX_CIP | AUX_BSY);
  regs[WD_AUX_STATUS]   |= AUX_INT;
  intrq = true;
  SPRINTF("sgi_scsi: complete status=0x%02x\n", scsi_status);
}

/* ---- command decode ------------------------------------------------------ */

void sgi_scsi::pause_transfer() {
  /* The HPC3 DMA descriptor chain ended (EOX) before this data phase finished:
   * IRIX deliberately programmed the WD33C93 transfer count for fewer bytes than
   * the SCSI command's full length (a chunked/scatter transfer). The real
   * WD33C93 raises an interrupt when its transfer count hits zero, leaving the
   * data phase active; IRIX then reprograms the DMA and re-issues SEL_ATN_XFER to
   * move the rest (see the resume path in select_and_transfer). Status/phase
   * codes match the validated iris WD33C93 model (ST_UNEX_SDATA on a data-in
   * read / ST_UNEX_RDATA on a data-out write). buf + pos are preserved. */
  drq = false;                            /* no data movement until resumed */
  regs[WD_SCSI_STATUS]   = (phase == PH_DATA_OUT) ? 0x48 : 0x49;
  regs[WD_COMMAND_PHASE] = 0x46;          /* transfer count exhausted */
  regs[0x12] = regs[0x13] = regs[0x14] = 0;  /* WD33C93 count counted down to 0 at the chunk boundary */
  regs[WD_AUX_STATUS]   &= ~(AUX_CIP | AUX_BSY);
  regs[WD_AUX_STATUS]   |= AUX_INT;
  intrq = true;
  SPRINTF("sgi_scsi: %s paused at %zu/%zu (chunked) -> INTRQ\n",
          phase == PH_DATA_OUT ? "data-out" : "data-in", pos, buf.size());
}

void sgi_scsi::select_and_transfer() {
  /* Resume a chunked transfer paused at a DMA EOX boundary: IRIX has
   * reprogrammed the DMA channel for the remaining bytes and re-issued
   * SEL_ATN_XFER. Continue the data phase from pos -- do NOT re-decode the CDB
   * (the HPC3 DMA pump after this command write moves the rest). */
  if((phase == PH_DATA_IN || phase == PH_DATA_OUT) && pos > 0 && pos < buf.size()) {
    SPRINTF("sgi_scsi: %s resume from %zu/%zu\n",
            phase == PH_DATA_OUT ? "data-out" : "data-in", pos, buf.size());
    regs[WD_AUX_STATUS] |= (AUX_CIP | AUX_BSY);
    drq = true;
    return;
  }

  regs[WD_AUX_STATUS] |= (AUX_CIP | AUX_BSY);
  const uint8_t *cdb = &regs[WD_CDB];
  const uint8_t op = cdb[0];
  uint8_t lun = regs[WD_TARGET_LUN] & 7;   /* capture before complete() overwrites 0x0f */
  SPRINTF("sgi_scsi: CDB %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x dest=%u lun=%u\n",
          cdb[0],cdb[1],cdb[2],cdb[3],cdb[4],cdb[5],cdb[6],cdb[7],cdb[8],cdb[9],
          regs[WD_DEST_ID]&7, lun);

  tgt_status = 0x00;                        /* GOOD unless we set CHECK below */

  /* Only LUN 0 exists. For any other LUN, return CHECK CONDITION with sense
   * key ILLEGAL REQUEST / ASC 0x25 (LOGICAL UNIT NOT SUPPORTED) and no data, so
   * the probe records "no device" and stops instead of looping. REQUEST SENSE
   * itself must still succeed so IRIX can read that sense. */
  if(lun != 0 && op != 0x03) {
    sense[0] = 0x70; sense[2] = 0x05; sense[7] = 0x0a; sense[12] = 0x25; sense[13] = 0x00;
    tgt_status = 0x02;                      /* CHECK CONDITION */
    finish();
    return;
  }

  switch(op) {
  case 0x00:   /* TEST UNIT READY  */
  case 0x1b:   /* START STOP UNIT  */
    finish();
    break;

  case 0x12: { /* INQUIRY */
    uint8_t inq[36] = {0};
    inq[0] = 0x00;            /* direct-access disk */
    inq[1] = 0x00;            /* not removable */
    inq[2] = 0x02;            /* SCSI-2 */
    inq[3] = 0x02;            /* response data format */
    inq[4] = 31;              /* additional length */
    memcpy(inq + 8,  "SGI     ", 8);
    memcpy(inq + 16, "interp_mips disk", 16);
    memcpy(inq + 32, "1.0 ", 4);
    size_t alloc = cdb[4];
    begin_data_in(inq, alloc < sizeof(inq) ? alloc : sizeof(inq));
    break;
  }

  case 0x03: { /* REQUEST SENSE */
    size_t alloc = cdb[4];
    begin_data_in(sense, alloc < sizeof(sense) ? alloc : sizeof(sense));
    break;
  }

  case 0x25: { /* READ CAPACITY(10) */
    uint8_t cap[8];
    uint32_t last = (nblocks ? (uint32_t)(nblocks - 1) : 0);
    cap[0]=last>>24; cap[1]=last>>16; cap[2]=last>>8; cap[3]=last;  /* last LBA, BE */
    cap[4]=0; cap[5]=0; cap[6]=2; cap[7]=0;                         /* block size 512, BE */
    begin_data_in(cap, sizeof(cap));
    break;
  }

  case 0x1a: { /* MODE SENSE(6) - minimal valid mode-parameter header */
    uint8_t ms[4] = {3, 0, 0, 0};   /* mode data len, medium type, dev-spec, blk-desc len */
    size_t alloc = cdb[4];
    begin_data_in(ms, alloc < sizeof(ms) ? alloc : sizeof(ms));
    break;
  }

  case 0x15: { /* MODE SELECT(6) - data-out, consume + succeed */
    begin_data_out(cdb[4]);
    break;
  }

  case 0x28: { /* READ(10) */
    uint64_t lba = ((uint64_t)cdb[2]<<24)|((uint64_t)cdb[3]<<16)|((uint64_t)cdb[4]<<8)|cdb[5];
    uint32_t len = ((uint32_t)cdb[7]<<8)|cdb[8];          /* blocks */
    buf.assign((size_t)len * 512, 0);
    for(uint32_t i = 0; i < len; i++)
      block_read(lba + i, buf.data() + (size_t)i * 512);
    pos = 0;
    if(len == 0) { finish(); break; }
    phase = PH_DATA_IN; drq = true;
    break;
  }

  case 0x2a: { /* WRITE(10) - data-out into COW overlay */
    uint64_t lba = ((uint64_t)cdb[2]<<24)|((uint64_t)cdb[3]<<16)|((uint64_t)cdb[4]<<8)|cdb[5];
    uint32_t len = ((uint32_t)cdb[7]<<8)|cdb[8];
    wr_lba = lba;
    begin_data_out((size_t)len * 512);
    break;
  }

  default:
    SPRINTF("sgi_scsi: UNHANDLED CDB opcode 0x%02x -> success/no-data\n", op);
    finish();
    break;
  }
}

void sgi_scsi::exec_command(uint8_t cc) {
  switch(cc) {
  case CMD_RESET:
    reset();
    regs[WD_SCSI_STATUS] = ST_RESET;
    regs[WD_AUX_STATUS] |= AUX_INT;
    intrq = true;
    break;
  case CMD_SEL_ATN_XFER:
  case CMD_SEL_XFER:
    select_and_transfer();
    break;
  case CMD_XFER_INFO:
    /* standalone Transfer-Info: data phase already armed by the prior command */
    break;
  default:
    SPRINTF("sgi_scsi: unhandled WD33C93 command 0x%02x\n", cc);
    break;
  }
}

/* ---- CPU PIO register file ------------------------------------------------ */

void sgi_scsi::pio_w(uint32_t port, uint8_t v) {
  SPRINTF("sgi_scsi: pio_w port=%u v=%02x (sasr=%02x)\n", port, v, sasr);
  if(port == 0) { sasr = v & 0x1f; return; }       /* SASR: set register pointer */
  /* SCMD: write the selected register */
  if(sasr == WD_COMMAND) { regs[WD_COMMAND] = v; exec_command(v & 0x7f); return; }
  regs[sasr] = v;
  sasr = (sasr + 1) & 0x1f;                         /* auto-increment */
}

uint8_t sgi_scsi::pio_r(uint32_t port) {
  if(port == 0) {                                   /* SASR read = aux status */
    uint8_t a = 0;
    if(intrq) a |= AUX_INT;
    if(drq)   a |= AUX_DBR;
    return a;
  }
  /* SCMD: read the selected register */
  if(sasr == WD_SCSI_STATUS) {                      /* reading status clears INTRQ */
    uint8_t st = regs[WD_SCSI_STATUS];
    intrq = false; regs[WD_AUX_STATUS] &= ~AUX_INT;
    sasr = (sasr + 1) & 0x1f;
    return st;
  }
  uint8_t r = regs[sasr];
  sasr = (sasr + 1) & 0x1f;
  return r;
}

/* ---- HPC3 DMA data byte port (driven by the data-phase FSM) --------------- */

uint8_t sgi_scsi::dma_r() {
  switch(phase) {
  case PH_DATA_IN: {
    if(pos >= buf.size()) return 0;
    uint8_t b = buf[pos++];
    if(pos >= buf.size()) finish();
    return b;
  }
  default:
    return 0;
  }
}

void sgi_scsi::dma_w(uint8_t v) {
  switch(phase) {
  case PH_DATA_OUT:
    if(pos >= buf.size()) return;
    buf[pos++] = v;
    if(pos >= buf.size()) finish();
    break;
  default:
    break;
  }
}
