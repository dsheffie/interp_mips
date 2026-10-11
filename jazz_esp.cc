#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "jazz_esp.hh"
#include "jazz.hh"

#ifdef __EMSCRIPTEN__
/* In the browser the disk is a sparse, in-memory image owned by JS (built from the uploaded
 * qcow2); these are implemented as EM_JS shims in wasm/em_main.cc.  Reads come from there;
 * writes still go to the C-side overlay below, so the upload is never modified. */
extern "C" {
  int      em_disk_present(void);
  unsigned em_disk_nblocks(void);
  void     em_disk_read(unsigned lba, uint8_t *dst);
}
#endif

/* register offsets (read / write names differ) */
static const uint32_t ESP_TCLO = 0x0, ESP_TCMID = 0x1, ESP_FIFO = 0x2, ESP_CMD = 0x3;
static const uint32_t ESP_RSTAT = 0x4, ESP_WBUSID = 0x4, ESP_RINTR = 0x5, ESP_RSEQ = 0x6;
static const uint32_t ESP_RFLAGS = 0x7, ESP_CFG1 = 0x8, ESP_TCHI = 0xe;
/* commands */
static const uint8_t CMD_DMA = 0x80, CMD_CMD = 0x7f;
static const uint8_t CMD_NOP = 0x00, CMD_FLUSH = 0x01, CMD_RESET = 0x02, CMD_BUSRESET = 0x03;
static const uint8_t CMD_TI = 0x10, CMD_ICCS = 0x11, CMD_MSGACC = 0x12, CMD_PAD = 0x18;
static const uint8_t CMD_SATN = 0x1a, CMD_RSTATN = 0x1b, CMD_SEL = 0x41, CMD_SELATN = 0x42;
static const uint8_t CMD_SELATNS = 0x43, CMD_ENSEL = 0x44, CMD_DISSEL = 0x45;
/* status register: phase in bits 2:0 */
static const uint8_t STAT_DO = 0, STAT_DI = 1, STAT_CD = 2, STAT_ST = 3, STAT_MO = 6, STAT_MI = 7;
static const uint8_t STAT_TC = 0x10, STAT_INT = 0x80;
/* interrupt register */
static const uint8_t INTR_FC = 0x08, INTR_BS = 0x10, INTR_DC = 0x20, INTR_IL = 0x40, INTR_RST = 0x80;
static const uint8_t SEQ_0 = 0x0, SEQ_MO = 0x1, SEQ_CD = 0x4;
static const uint8_t CFG1_RESREPT = 0x40;
static const uint8_t TCHI_FAS100A = 0x4;       /* what QEMU's sysbus ESP reports before TCHI is written */
static const uint8_t ASC_MODE_DIS = 0, ASC_MODE_INI = 1;
static const uint32_t FIFO_SZ = 16, CMDFIFO_SZ = 32;
static const uint8_t DISK_ID = 0;
/* SCSI status / sense */
static const uint8_t GOOD = 0x00, CHECK_CONDITION = 0x02;
static const uint8_t NOT_READY = 0x2, ILLEGAL_REQUEST = 0x5, UNIT_ATTENTION = 0x6;

jazz_esp::jazz_esp(jazz *jz, const std::string &disk_path, bool disk_write)
  : jz(jz), direct_write(disk_write) {
  hard_reset();
#ifdef __EMSCRIPTEN__
  if(em_disk_present()) {
    nblocks = em_disk_nblocks();               /* full virtual geometry; reads served from JS */
    fprintf(stderr, "[jazz] esp: SCSI ID 0 = sparse upload, %lu blocks [copy-on-write]\n",
            (unsigned long)nblocks);
  }
  else
#endif
  if(not disk_path.empty()) {
    fd = open(disk_path.c_str(), disk_write ? O_RDWR : O_RDONLY);
    if(fd < 0) {
      fprintf(stderr, "[jazz] esp: cannot open disk image '%s'\n", disk_path.c_str());
    }
    else {
      struct stat st;
      if(fstat(fd, &st) == 0) {
        nblocks = (uint64_t)st.st_size / 512;
      }
      fprintf(stderr, "[jazz] esp: SCSI ID 0 = '%s', %lu blocks%s\n", disk_path.c_str(),
              (unsigned long)nblocks, disk_write ? " [writes go to the image]" : " [copy-on-write]");
    }
  }
  unit_attention = true;                       /* power-on reset */
}

jazz_esp::~jazz_esp() {
  if(fd >= 0) {
    close(fd);
  }
}

bool jazz_esp::irq() const {
  return (rregs[ESP_RSTAT] & STAT_INT) != 0;
}

/* ---- small helpers (QEMU esp_get_tc / esp_set_tc / fifo8) ---- */
uint32_t jazz_esp::get_tc() const {
  return rregs[ESP_TCLO] | (rregs[ESP_TCMID] << 8) | (rregs[ESP_TCHI] << 16);
}

void jazz_esp::set_tc(uint32_t v) {
  uint32_t old_tc = get_tc();
  rregs[ESP_TCLO] = v & 0xff;
  rregs[ESP_TCMID] = (v >> 8) & 0xff;
  rregs[ESP_TCHI] = (v >> 16) & 0xff;
  if(old_tc and v == 0) {
    rregs[ESP_RSTAT] |= STAT_TC;
  }
}

uint32_t jazz_esp::get_stc() const {
  return wregs[ESP_TCLO] | (wregs[ESP_TCMID] << 8) | (wregs[ESP_TCHI] << 16);
}

void jazz_esp::set_phase(uint8_t p) {
  rregs[ESP_RSTAT] = (rregs[ESP_RSTAT] & ~7) | p;
}

void jazz_esp::raise_irq() {
  rregs[ESP_RSTAT] |= STAT_INT;
}

void jazz_esp::lower_irq() {
  rregs[ESP_RSTAT] &= ~STAT_INT;
}

void jazz_esp::fifo_push(uint8_t v) {
  if(fifo.size() < FIFO_SZ) {
    fifo.push_back(v);
  }
}

uint32_t jazz_esp::fifo_pop_buf(uint8_t *dst, uint32_t maxlen) {
  uint32_t n = 0;
  while(n < maxlen and not fifo.empty()) {
    dst[n++] = fifo.front();
    fifo.pop_front();
  }
  return n;
}

void jazz_esp::cmdfifo_push(const uint8_t *src, uint32_t len) {
  for(uint32_t i = 0; i < len and cmdfifo.size() < CMDFIFO_SZ; i++) {
    cmdfifo.push_back(src[i]);
  }
}

/* ---- selection / command / message phases ---- */
int jazz_esp::select() {
  uint8_t target = wregs[ESP_WBUSID] & 7;
  ti_size = 0;
  rregs[ESP_RSEQ] = SEQ_0;
  if(current_req) {
    cancel_request();                          /* a new command before the old one finished */
  }
  if(target != DISK_ID or nblocks == 0) {      /* no such target: selection timeout.  Test nblocks,
                                                * not fd: the wasm build has no file descriptor (the disk
                                                * is the in-memory sparse store), yet the disk IS present. */
    rregs[ESP_RSTAT] = 0;
    asc_mode = ASC_MODE_DIS;
    rregs[ESP_RINTR] = INTR_DC;
    raise_irq();
    return -1;
  }
  current_dev = true;
  asc_mode = ASC_MODE_INI;                     /* IRQ comes from transfer_data / command_complete */
  return 0;
}

void jazz_esp::do_command_phase() {
  uint32_t cmdlen = cmdfifo.size();
  if(cmdlen == 0 or not current_dev) {
    return;
  }
  uint8_t buf[CMDFIFO_SZ];
  for(uint32_t i = 0; i < cmdlen; i++) {
    buf[i] = cmdfifo[i];
  }
  cmdfifo.clear();
  current_req = true;
  int32_t datalen = disk_command(buf, cmdlen);
  ti_size = datalen;
  data_ready = false;
  if(datalen != 0) {
    /* DATA phase, but the command-completion interrupt waits for the first data transfer */
    set_phase(datalen > 0 ? STAT_DI : STAT_DO);
    req_continue();
  }
}

void jazz_esp::do_message_phase() {
  if(cmdfifo_cdb_offset) {
    uint8_t message = 0;
    if(not cmdfifo.empty()) {
      message = cmdfifo.front();
      cmdfifo.pop_front();
    }
    lun = message & 7;                         /* IDENTIFY */
    cmdfifo_cdb_offset--;
  }
  if(cmdfifo_cdb_offset) {                     /* extended messages are ignored */
    uint32_t n = std::min<uint32_t>(cmdfifo_cdb_offset, cmdfifo.size());
    cmdfifo.erase(cmdfifo.begin(), cmdfifo.begin() + n);
    cmdfifo_cdb_offset = 0;
  }
}

void jazz_esp::do_cmd() {
  do_message_phase();
  do_command_phase();
}

bool jazz_esp::cdb_ready() {
  uint32_t len = cmdfifo.size();
  if(len == 0 or len < (uint32_t)cmdfifo_cdb_offset + 1) {
    return false;
  }
  int cdblen;
  switch(cmdfifo[cmdfifo_cdb_offset] >> 5)
    {
    case 0:
      cdblen = 6;
      break;
    case 1:
    case 2:
      cdblen = 10;
      break;
    case 4:
      cdblen = 16;
      break;
    case 5:
      cdblen = 12;
      break;
    default:
      cdblen = -1;
      break;
    }
  return cdblen < 0 ? false : (len >= (uint32_t)cdblen);
}

void jazz_esp::dma_ti_check() {
  if(get_tc() == 0 and fifo.size() < 2) {
    rregs[ESP_RINTR] |= INTR_BS;
    raise_irq();
  }
}

/* ---- DMA transfers (QEMU esp_do_dma with dma_memory_read/write = rc4030 channel 0) ---- */
void jazz_esp::do_dma() {
  uint32_t len = get_tc();
  uint8_t buf[CMDFIFO_SZ];
  switch(get_phase())
    {
    case STAT_MO:
      len = std::min<uint32_t>(len, CMDFIFO_SZ - cmdfifo.size());
      if(len) {
        jz->dma(0, buf, len, false);
        set_tc(get_tc() - len);
      }
      cmdfifo_push(buf, len);
      cmdfifo_cdb_offset += len;
      switch(rregs[ESP_CMD])
        {
        case CMD_SELATN | CMD_DMA:
          if(cmdfifo.size() >= 1) {              /* first byte received: on to COMMAND */
            set_phase(STAT_CD);
            rregs[ESP_RSEQ] = SEQ_CD;
            cmdfifo_cdb_offset = 1;
            if(cmdfifo.size() > 1) {
              do_dma();
            }
          }
          break;
        case CMD_SELATNS | CMD_DMA:
          if(cmdfifo.size() == 1) {              /* first byte received: stop in MESSAGE OUT */
            rregs[ESP_RSEQ] = SEQ_MO;
            cmdfifo_cdb_offset = 1;
            rregs[ESP_RINTR] |= INTR_BS | INTR_FC;
            raise_irq();
          }
          break;
        case CMD_TI | CMD_DMA:
          if(get_tc() == 0) {                    /* ATN stays asserted until TC == 0 */
            set_phase(STAT_CD);
            rregs[ESP_CMD] = 0;
            rregs[ESP_RINTR] |= INTR_BS;
            raise_irq();
          }
          break;
        default:
          break;
        }
      break;
    case STAT_CD:
      len = std::min<uint32_t>(len, CMDFIFO_SZ - cmdfifo.size());
      if(len) {
        jz->dma(0, buf, len, false);
        cmdfifo_push(buf, len);
        set_tc(get_tc() - len);
      }
      ti_size = 0;
      if(get_tc() == 0) {
        do_cmd();
      }
      break;
    case STAT_DO:
      if(not current_req) {
        return;
      }
      if(async_len == 0 and get_tc()) {
        return;                                  /* defer until data is available */
      }
      len = std::min(len, async_len);
      switch(rregs[ESP_CMD])
        {
        case CMD_TI | CMD_DMA:
          if(len) {
            jz->dma(0, req_buf.data() + async_pos, len, false);
            set_tc(get_tc() - len);
          }
          async_pos += len;
          async_len -= len;
          ti_size += len;
          break;
        case CMD_PAD | CMD_DMA:
          memset(req_buf.data() + async_pos, 0, len);
          async_pos += len;
          async_len -= len;
          ti_size += len;
          break;
        default:
          break;
        }
      if(async_len == 0 and fifo.size() < 2) {
        req_continue();
        return;
      }
      dma_ti_check();
      break;
    case STAT_DI:
      if(not current_req) {
        return;
      }
      if(async_len == 0 and get_tc()) {
        return;
      }
      len = std::min(len, async_len);
      switch(rregs[ESP_CMD])
        {
        case CMD_TI | CMD_DMA:
          if(len) {
            jz->dma(0, req_buf.data() + async_pos, len, true);
          }
          async_pos += len;
          async_len -= len;
          ti_size -= len;
          set_tc(get_tc() - len);
          break;
        case CMD_PAD | CMD_DMA:
          async_pos += len;
          async_len -= len;
          ti_size -= len;
          set_tc(get_tc() - len);
          break;
        default:
          break;
        }
      if(async_len == 0 and ti_size == 0 and get_tc()) {
        req_continue();                          /* the guest underflowed TC: finish the request */
        return;
      }
      if(async_len == 0 and fifo.size() < 2) {
        req_continue();
        return;
      }
      dma_ti_check();
      break;
    case STAT_ST:
      if(rregs[ESP_CMD] == (CMD_ICCS | CMD_DMA)) {
        len = std::min<uint32_t>(len, 1);
        if(len) {
          buf[0] = status;
          jz->dma(0, buf, len, true);
          set_tc(get_tc() - len);
          set_phase(STAT_MI);
          if(get_tc() > 0) {
            do_dma();
          }
        }
      }
      else if(fifo.size() < 2) {
        rregs[ESP_RINTR] |= INTR_BS;
        raise_irq();
      }
      break;
    case STAT_MI:
      if(rregs[ESP_CMD] == (CMD_ICCS | CMD_DMA)) {
        len = std::min<uint32_t>(len, 1);
        if(len) {
          buf[0] = 0;                            /* COMMAND COMPLETE */
          jz->dma(0, buf, len, true);
          set_tc(get_tc() - len);
          rregs[ESP_RINTR] |= INTR_FC;
          raise_irq();
        }
      }
      break;
    default:
      break;
    }
}

void jazz_esp::nodma_ti_dataout() {
  if(not current_req or async_len == 0) {
    return;
  }
  uint32_t len = std::min<uint32_t>(async_len, FIFO_SZ);
  len = fifo_pop_buf(req_buf.data() + async_pos, std::min<uint32_t>(len, fifo.size()));
  async_pos += len;
  async_len -= len;
  ti_size += len;
  if(async_len == 0) {
    req_continue();
    return;
  }
  rregs[ESP_RINTR] |= INTR_BS;
  raise_irq();
}

/* ---- programmed-I/O transfers (QEMU esp_do_nodma) ---- */
void jazz_esp::do_nodma() {
  uint8_t buf[FIFO_SZ];
  uint32_t len;
  switch(get_phase())
    {
    case STAT_MO:
      switch(rregs[ESP_CMD])
        {
        case CMD_SELATN:
          len = fifo_pop_buf(buf, fifo.size());
          cmdfifo_push(buf, len);
          if(cmdfifo.size() >= 1) {
            set_phase(STAT_CD);
            rregs[ESP_RSEQ] = SEQ_CD;
            cmdfifo_cdb_offset = 1;
            if(cmdfifo.size() > 1) {
              do_nodma();
            }
          }
          break;
        case CMD_SELATNS:
          len = fifo_pop_buf(buf, std::min<uint32_t>(fifo.size(), 1));
          cmdfifo_push(buf, len);
          if(cmdfifo.size() >= 1) {
            rregs[ESP_RSEQ] = SEQ_MO;
            cmdfifo_cdb_offset = 1;
            rregs[ESP_RINTR] |= INTR_BS | INTR_FC;
            raise_irq();
          }
          break;
        case CMD_TI:
          len = fifo_pop_buf(buf, fifo.size());
          cmdfifo_push(buf, len);
          cmdfifo_cdb_offset = cmdfifo.size();   /* ATN stays asserted until the FIFO is empty */
          set_phase(STAT_CD);
          rregs[ESP_CMD] = 0;
          rregs[ESP_RINTR] |= INTR_BS;
          raise_irq();
          break;
        default:
          break;
        }
      break;
    case STAT_CD:
      switch(rregs[ESP_CMD])
        {
        case CMD_TI:
          len = fifo_pop_buf(buf, fifo.size());
          cmdfifo_push(buf, len);
          if(cdb_ready()) {                      /* the CDB may take several TI commands */
            do_cmd();
          }
          else if(len) {
            rregs[ESP_RINTR] |= INTR_BS;
            raise_irq();
          }
          break;
        case CMD_SEL | CMD_DMA:
        case CMD_SELATN | CMD_DMA:
          len = fifo_pop_buf(buf, fifo.size());   /* DMA selection finished by FIFO writes */
          cmdfifo_push(buf, len);
          if(cdb_ready()) {
            do_cmd();
          }
          break;
        case CMD_SEL:
        case CMD_SELATN:
          len = fifo_pop_buf(buf, fifo.size());   /* the FIFO already holds the whole CDB */
          cmdfifo_push(buf, len);
          do_cmd();
          break;
        default:
          break;
        }
      break;
    case STAT_DO:
      break;                                     /* data accumulates in the FIFO until TI */
    case STAT_DI:
      if(not current_req or async_len == 0) {
        return;
      }
      if(fifo.empty()) {
        fifo_push(req_buf[async_pos]);
        async_pos++;
        async_len--;
        ti_size--;
      }
      if(async_len == 0) {
        req_continue();
        return;
      }
      if(rregs[ESP_CMD] != CMD_TI) {
        return;                                  /* preloading the FIFO: wait for TI */
      }
      rregs[ESP_RINTR] |= INTR_BS;
      raise_irq();
      break;
    case STAT_ST:
      if(rregs[ESP_CMD] == CMD_ICCS) {
        fifo_push(status);
        set_phase(STAT_MI);
        do_nodma();
      }
      break;
    case STAT_MI:
      if(rregs[ESP_CMD] == CMD_ICCS) {
        fifo_push(0);                            /* COMMAND COMPLETE */
        rregs[ESP_RINTR] |= INTR_FC;
        raise_irq();
      }
      break;
    default:
      break;
    }
}

/* ---- the SCSI-layer callbacks (QEMU scsi_req_continue -> esp_transfer_data /
 * esp_command_complete), run synchronously ---- */
void jazz_esp::req_continue() {
  if(not current_req) {
    return;
  }
  if(not req_data_done) {
    req_data_done = true;
    async_pos = 0;
    transfer_data(req_buf.size());
    return;
  }
  if(req_is_write) {
    for(uint32_t i = 0; i < req_buf.size() / 512; i++) {
      block_write(req_lba + i, req_buf.data() + i * 512);
    }
  }
  command_complete(req_status);
}

void jazz_esp::transfer_data(uint32_t len) {
  async_len = len;
  if(not data_ready) {
    data_ready = true;
    switch(rregs[ESP_CMD])
      {
      case CMD_SEL | CMD_DMA:
      case CMD_SEL:
      case CMD_SELATN | CMD_DMA:
      case CMD_SELATN:
        rregs[ESP_RINTR] |= INTR_BS | INTR_FC;   /* sequencer command done, data phase next */
        rregs[ESP_RSEQ] = SEQ_CD;
        raise_irq();
        break;
      case CMD_SELATNS | CMD_DMA:
      case CMD_SELATNS:
        rregs[ESP_RINTR] |= INTR_BS;
        rregs[ESP_RSEQ] = SEQ_MO;
        raise_irq();
        break;
      case CMD_TI | CMD_DMA:
      case CMD_TI:
        rregs[ESP_CMD] = 0;
        rregs[ESP_RINTR] |= INTR_BS;
        raise_irq();
        break;
      default:
        break;
      }
  }
  if(rregs[ESP_CMD] == (CMD_TI | CMD_DMA)) {
    dma_ti_check();
    do_dma();
  }
  else if(rregs[ESP_CMD] == CMD_TI) {
    do_nodma();
  }
}

void jazz_esp::command_complete(uint8_t st) {
  async_len = 0;
  status = st;
  ti_size = 0;
  switch(rregs[ESP_CMD])
    {
    case CMD_SEL | CMD_DMA:
    case CMD_SEL:
    case CMD_SELATN | CMD_DMA:
    case CMD_SELATN:
      rregs[ESP_RINTR] |= INTR_BS | INTR_FC;     /* no data phase: deferred BS + FC */
      rregs[ESP_RSEQ] = SEQ_CD;
      break;
    case CMD_TI | CMD_DMA:
    case CMD_TI:
      rregs[ESP_CMD] = 0;
      break;
    default:
      break;
    }
  set_phase(STAT_ST);                            /* bus service: on to STATUS */
  rregs[ESP_RINTR] |= INTR_BS;
  raise_irq();
  current_req = false;
  current_dev = false;
}

void jazz_esp::cancel_request() {
  current_req = false;
  current_dev = false;
  async_len = 0;
}

void jazz_esp::handle_ti() {
  if(dma) {
    do_dma();
  }
  else {
    do_nodma();
    if(get_phase() == STAT_DO) {
      nodma_ti_dataout();
    }
  }
}

void jazz_esp::hard_reset() {
  memset(rregs, 0, sizeof(rregs));
  memset(wregs, 0, sizeof(wregs));
  tchi_written = false;
  ti_size = 0;
  async_len = 0;
  fifo.clear();
  cmdfifo.clear();
  dma = false;
  asc_mode = ASC_MODE_DIS;
  rregs[ESP_CFG1] = 7;
}

void jazz_esp::bus_reset() {
  if(current_req) {
    cancel_request();
  }
  unit_attention = true;                         /* the disk saw a SCSI bus reset */
  sense_is_ua = false;
}

bool jazz_esp::cmd_is_valid(uint8_t cmd) const {
  uint8_t group = (cmd & 0x70) >> 4;
  if(group == 0) {
    return true;                                 /* miscellaneous commands are always allowed */
  }
  if(asc_mode == ASC_MODE_DIS) {
    return group == 4;                           /* disconnected-state commands */
  }
  return group == 1;                             /* initiator commands */
}

void jazz_esp::run_cmd() {
  uint8_t cmd = rregs[ESP_CMD];
  if(cmd & CMD_DMA) {
    dma = true;
    set_tc(get_stc() == 0 ? 0x10000 : get_stc());   /* reload the counter */
  }
  else {
    dma = false;
  }
  switch(cmd & CMD_CMD)
    {
    case CMD_NOP:
      break;
    case CMD_FLUSH:
      fifo.clear();
      break;
    case CMD_RESET:
      hard_reset();
      break;
    case CMD_BUSRESET:
      bus_reset();
      if(not(wregs[ESP_CFG1] & CFG1_RESREPT)) {
        rregs[ESP_RINTR] |= INTR_RST;
        raise_irq();
      }
      break;
    case CMD_TI:
      handle_ti();
      break;
    case CMD_ICCS:
      if(dma) {
        do_dma();
      }
      else {
        do_nodma();
      }
      break;
    case CMD_MSGACC:
      asc_mode = ASC_MODE_DIS;
      rregs[ESP_RINTR] |= INTR_DC;
      rregs[ESP_RSEQ] = 0;
      rregs[ESP_RFLAGS] = 0;
      raise_irq();
      break;
    case CMD_PAD:
      if(dma) {
        do_dma();
      }
      else {
        do_nodma();
      }
      break;
    case CMD_SATN:
    case CMD_RSTATN:
      break;
    case CMD_SEL:
      if(select() == 0) {
        set_phase(STAT_CD);
        cmdfifo_cdb_offset = 0;
        if(dma) {
          do_dma();
        }
        else {
          do_nodma();
        }
      }
      break;
    case CMD_SELATN:
      if(select() == 0) {
        set_phase(STAT_MO);
        if(dma) {
          do_dma();
        }
        else {
          do_nodma();
        }
      }
      break;
    case CMD_SELATNS:
      if(select() == 0) {
        set_phase(STAT_MO);
        cmdfifo_cdb_offset = 0;
        if(dma) {
          do_dma();
        }
        else {
          do_nodma();
        }
      }
      break;
    case CMD_ENSEL:
      rregs[ESP_RINTR] = 0;
      break;
    case CMD_DISSEL:
      rregs[ESP_RINTR] = 0;
      raise_irq();
      break;
    default:
      fprintf(stderr, "[jazz] esp: unhandled command %02x\n", cmd);
      break;
    }
}

uint8_t jazz_esp::read(uint32_t o) {
  uint8_t v;
  switch(o)
    {
    case ESP_FIFO:
      v = 0;
      if(not fifo.empty()) {
        v = fifo.front();
        fifo.pop_front();
      }
      rregs[ESP_FIFO] = v;
      break;
    case ESP_RINTR:
      /* clears the interrupt register and every status bit except TC and the phase (QEMU keeps
       * the sequence step, see esp_reg_read) */
      v = rregs[ESP_RINTR];
      rregs[ESP_RINTR] = 0;
      lower_irq();
      rregs[ESP_RSTAT] &= STAT_TC | 7;
      break;
    case ESP_TCHI:
      v = tchi_written ? rregs[ESP_TCHI] : TCHI_FAS100A;
      break;
    case ESP_RFLAGS:
      v = fifo.size();                           /* bits 4:0 = FIFO byte count */
      break;
    default:
      v = rregs[o & 15];
      break;
    }
  return v;
}

void jazz_esp::write(uint32_t o, uint8_t v) {
  o &= 15;
  switch(o)
    {
    case ESP_TCHI:
      tchi_written = true;
      rregs[ESP_RSTAT] &= ~STAT_TC;
      break;
    case ESP_TCLO:
    case ESP_TCMID:
      rregs[ESP_RSTAT] &= ~STAT_TC;
      break;
    case ESP_FIFO:
      fifo_push(v);
      do_nodma();
      break;
    case ESP_CMD:
      rregs[ESP_CMD] = v;
      if(not cmd_is_valid(v)) {
        rregs[ESP_RSTAT] |= INTR_IL;             /* as QEMU: the IL bit lands in the status register */
        raise_irq();
        break;
      }
      run_cmd();
      break;
    case 0x8:                                    /* CFG1 */
    case 0xb:                                    /* CFG2 */
    case 0xc:                                    /* CFG3 */
    case 0xd:
    case 0xf:
      rregs[o] = v;
      break;
    default:                                     /* bus ID, select timeout, sync period/offset, clock, test */
      break;
    }
  wregs[o] = v;
}

/* ---- the disk target (SCSI ID 0) ---- */
int32_t jazz_esp::check_condition(uint8_t key, uint8_t asc, uint8_t ascq) {
  sense_key = key;
  sense_asc = asc;
  sense_ascq = ascq;
  command_complete(CHECK_CONDITION);
  return 0;
}

int32_t jazz_esp::data_in(const uint8_t *src, uint32_t len, uint32_t alloc) {
  len = std::min(len, alloc);
  if(len == 0) {
    command_complete(GOOD);
    return 0;
  }
  req_buf.assign(src, src + len);
  req_out = false;
  req_is_write = false;
  req_data_done = false;
  req_status = GOOD;
  return (int32_t)len;
}

uint32_t jazz_esp::mode_page(uint8_t page, uint8_t pc, uint8_t *p) {
  /* returns the page length including its 2-byte header; pc 1 (changeable) = all zero */
  uint32_t cyls = (uint32_t)(nblocks / (16 * 63));
  switch(page)
    {
    case 0x01:                                   /* read/write error recovery */
      p[0] = 0x01;
      p[1] = 10;
      if(pc != 1) {
        p[2] = 0x80;                             /* AWRE */
      }
      return 12;
    case 0x04:                                   /* rigid disk geometry */
      p[0] = 0x04;
      p[1] = 0x16;
      if(pc != 1) {
        p[2] = (cyls >> 16) & 0xff;
        p[3] = (cyls >> 8) & 0xff;
        p[4] = cyls & 0xff;
        p[5] = 16;
        p[6] = p[9] = (cyls >> 16) & 0xff;
        p[7] = p[10] = (cyls >> 8) & 0xff;
        p[8] = p[11] = cyls & 0xff;
        p[13] = 200;
        p[14] = p[15] = p[16] = 0xff;
        p[20] = 5400 >> 8;
        p[21] = 5400 & 0xff;
      }
      return 0x18;
    case 0x08:                                   /* caching: write cache enabled */
      p[0] = 0x08;
      p[1] = 0x12;
      p[2] = 0x04;
      return 0x14;
    default:
      return 0;
    }
}

int32_t jazz_esp::disk_command(const uint8_t *cdb, uint32_t cdblen) {
  (void)cdblen;
  uint8_t op = cdb[0];
  uint8_t out[512];
  memset(out, 0, sizeof(out));
  /* a pending unit attention fails everything but INQUIRY / REPORT LUNS / its REQUEST SENSE */
  if(unit_attention and op != 0x12 and op != 0xa0 and not(op == 0x03 and sense_is_ua)) {
    unit_attention = false;
    sense_is_ua = true;
    return check_condition(UNIT_ATTENTION, 0x29, 0x00);
  }
  if(op == 0x03) {                               /* REQUEST SENSE (fixed format) */
    out[0] = 0x70;
    out[7] = 10;
    if(lun != 0) {
      out[2] = ILLEGAL_REQUEST;
      out[12] = 0x25;
    }
    else {
      out[2] = sense_key;
      out[12] = sense_asc;
      out[13] = sense_ascq;
    }
    sense_key = sense_asc = sense_ascq = 0;
    sense_is_ua = false;
    return data_in(out, 18, cdb[4]);
  }
  if(lun != 0) {                                 /* only LUN 0 exists */
    if(op == 0x12) {
      out[0] = 0x7f;                             /* peripheral qualifier 3: no device on this LUN */
      out[2] = 5;
      out[3] = 0x12;
      out[4] = 31;
      return data_in(out, 36, (cdb[3] << 8) | cdb[4]);
    }
    return check_condition(ILLEGAL_REQUEST, 0x25, 0x00);
  }
  sense_key = sense_asc = sense_ascq = 0;
  uint64_t lba = 0;
  uint32_t count = 0;
  switch(op)
    {
    case 0x00:                                   /* TEST UNIT READY */
    case 0x1b:                                   /* START STOP UNIT */
    case 0x1e:                                   /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x16:                                   /* RESERVE */
    case 0x17:                                   /* RELEASE */
    case 0x2f:                                   /* VERIFY(10) */
    case 0x35:                                   /* SYNCHRONIZE CACHE */
      command_complete(GOOD);
      return 0;
    case 0x12: {                                 /* INQUIRY */
      uint32_t alloc = (cdb[3] << 8) | cdb[4];
      if(cdb[1] & 1) {                           /* EVPD: only the supported-pages page */
        if(cdb[2] != 0) {
          return check_condition(ILLEGAL_REQUEST, 0x24, 0x00);
        }
        out[3] = 1;
        return data_in(out, 5, alloc);
      }
      out[2] = 5;                                /* SPC-3 */
      out[3] = 0x12;                             /* HiSup, response data format 2 */
      out[4] = 31;
      out[7] = 0x10;                             /* synchronous transfers */
      memcpy(&out[8], "ISS     ", 8);
      memcpy(&out[16], "JAZZ DISK       ", 16);
      memcpy(&out[32], "1.0 ", 4);
      return data_in(out, 36, alloc);
    }
    case 0xa0: {                                 /* REPORT LUNS: just LUN 0 */
      uint32_t alloc = (cdb[6] << 24) | (cdb[7] << 16) | (cdb[8] << 8) | cdb[9];
      out[3] = 8;
      return data_in(out, 16, alloc);
    }
    case 0x25: {                                 /* READ CAPACITY(10) */
      uint64_t last = nblocks ? nblocks - 1 : 0;
      if(last > 0xffffffffULL) {
        last = 0xffffffffULL;
      }
      out[0] = last >> 24;
      out[1] = last >> 16;
      out[2] = last >> 8;
      out[3] = last;
      out[6] = 512 >> 8;
      return data_in(out, 8, 8);
    }
    case 0x1a:                                   /* MODE SENSE(6) */
    case 0x5a: {                                 /* MODE SENSE(10) */
      bool ten = op == 0x5a;
      bool dbd = (cdb[1] & 0x08) != 0;
      uint8_t pc = cdb[2] >> 6, page = cdb[2] & 0x3f;
      uint32_t alloc = ten ? ((cdb[7] << 8) | cdb[8]) : cdb[4];
      if(pc == 3) {
        return check_condition(ILLEGAL_REQUEST, 0x39, 0x00);   /* saving parameters not supported */
      }
      uint32_t n = ten ? 8 : 4;
      if(not dbd) {
        uint64_t nb = nblocks > 0xffffff ? 0 : nblocks;
        out[n + 1] = nb >> 16;
        out[n + 2] = nb >> 8;
        out[n + 3] = nb;
        out[n + 6] = 512 >> 8;
        n += 8;
        out[ten ? 7 : 3] = 8;                    /* block descriptor length */
      }
      if(page == 0x3f) {
        n += mode_page(0x01, pc, &out[n]);
        n += mode_page(0x04, pc, &out[n]);
        n += mode_page(0x08, pc, &out[n]);
      }
      else {
        uint32_t len = mode_page(page, pc, &out[n]);
        if(len == 0) {
          return check_condition(ILLEGAL_REQUEST, 0x24, 0x00);
        }
        n += len;
      }
      if(ten) {
        out[0] = (n - 2) >> 8;
        out[1] = (n - 2) & 0xff;
      }
      else {
        out[0] = n - 1;
      }
      return data_in(out, n, alloc);
    }
    case 0x15:                                   /* MODE SELECT(6): accept and ignore */
    case 0x55: {                                 /* MODE SELECT(10) */
      uint32_t len = op == 0x15 ? cdb[4] : ((cdb[7] << 8) | cdb[8]);
      if(len == 0) {
        command_complete(GOOD);
        return 0;
      }
      req_buf.assign(len, 0);
      req_out = true;
      req_is_write = false;                      /* parameter data is dropped, never written to disk */
      req_data_done = false;
      req_status = GOOD;
      return -(int32_t)len;
    }
    case 0x08:                                   /* READ(6) */
    case 0x0a:                                   /* WRITE(6) */
      lba = ((cdb[1] & 0x1f) << 16) | (cdb[2] << 8) | cdb[3];
      count = cdb[4] ? cdb[4] : 256;
      break;
    case 0x28:                                   /* READ(10) */
    case 0x2a:                                   /* WRITE(10) */
      lba = ((uint32_t)cdb[2] << 24) | (cdb[3] << 16) | (cdb[4] << 8) | cdb[5];
      count = (cdb[7] << 8) | cdb[8];
      break;
    default:
      fprintf(stderr, "[jazz] esp: disk opcode %02x not supported\n", op);
      return check_condition(ILLEGAL_REQUEST, 0x20, 0x00);
    }
  /* READ / WRITE */
  if(lba + count > nblocks) {
    return check_condition(ILLEGAL_REQUEST, 0x21, 0x00);
  }
  if(count == 0) {
    command_complete(GOOD);
    return 0;
  }
  bool is_write = op == 0x0a or op == 0x2a;
  req_buf.resize(count * 512);
  req_out = is_write;
  req_is_write = is_write;
  req_data_done = false;
  req_status = GOOD;
  req_lba = lba;
  if(not is_write) {
    for(uint32_t i = 0; i < count; i++) {
      block_read(lba + i, req_buf.data() + i * 512);
    }
    return (int32_t)(count * 512);
  }
  return -(int32_t)(count * 512);
}

void jazz_esp::block_read(uint64_t lba, uint8_t *dst) {
  auto it = overlay.find(lba);
  if(it != overlay.end()) {
    memcpy(dst, it->second.data(), 512);
    return;
  }
#ifdef __EMSCRIPTEN__
  em_disk_read((unsigned)lba, dst);            /* base image from the JS sparse store (zeros if unmapped) */
  return;
#else
  if(pread(fd, dst, 512, (off_t)(lba * 512)) != 512) {
    memset(dst, 0, 512);
  }
#endif
}

void jazz_esp::block_write(uint64_t lba, const uint8_t *src) {
  if(direct_write) {
    if(pwrite(fd, src, 512, (off_t)(lba * 512)) != 512) {
      fprintf(stderr, "[jazz] esp: write of block %lu failed\n", (unsigned long)lba);
    }
    return;
  }
  overlay[lba].assign(src, src + 512);
}
