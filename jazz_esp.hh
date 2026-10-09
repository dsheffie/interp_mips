#ifndef __JAZZ_ESP_HH__
#define __JAZZ_ESP_HH__

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

class jazz;

/* jazz_esp: the Jazz NCR 53C94 (ESP) SCSI controller at 0x80002000 (byte registers 0..15), DMA
 * through R4030 channel 0, interrupting as local source 5, with one direct-access disk at SCSI
 * ID 0 backed by a raw image.  The register / phase / interrupt behaviour is a synchronous port
 * of QEMU hw/scsi/esp.c -- the controller NT 4.0's driver boots against in the J0 runs -- and
 * the disk target follows QEMU scsi-bus.c / scsi-disk.c (RESET unit attention after power-on and
 * bus reset, LUN != 0 handling, mode pages 1/4/8).  "Synchronous": the disk answers every
 * command immediately, so data is always ready by the time the guest issues TRANSFER INFO.
 * Writes land in an in-memory copy-on-write overlay unless disk_write (then in the image). */
class jazz_esp {
public:
  jazz_esp(jazz *jz, const std::string &disk_path, bool disk_write);
  ~jazz_esp();
  uint8_t read(uint32_t o);
  void write(uint32_t o, uint8_t v);
  bool irq() const;
private:
  jazz *jz;
  /* ESP */
  uint8_t rregs[16], wregs[16];
  bool tchi_written = false;
  std::deque<uint8_t> fifo, cmdfifo;           /* 16-byte data FIFO, 32-byte command buffer */
  uint8_t cmdfifo_cdb_offset = 0, lun = 0, asc_mode = 0;
  bool dma = false, data_ready = false;
  int32_t ti_size = 0;
  uint8_t status = 0;
  /* the current request (QEMU current_dev / current_req / async_buf) */
  bool current_dev = false, current_req = false;
  std::vector<uint8_t> req_buf;
  uint32_t async_pos = 0, async_len = 0;
  bool req_out = false, req_is_write = false, req_data_done = false;
  uint8_t req_status = 0;
  uint64_t req_lba = 0;
  /* disk (SCSI ID 0, LUN 0) */
  int fd = -1;
  uint64_t nblocks = 0;
  bool direct_write;
  std::unordered_map<uint64_t, std::vector<uint8_t>> overlay;
  uint8_t sense_key = 0, sense_asc = 0, sense_ascq = 0;
  bool sense_is_ua = false, unit_attention = false;

  uint32_t get_tc() const;
  void set_tc(uint32_t v);
  uint32_t get_stc() const;
  uint8_t get_phase() const { return rregs[4] & 7; }
  void set_phase(uint8_t p);
  void raise_irq();
  void lower_irq();
  void fifo_push(uint8_t v);
  uint32_t fifo_pop_buf(uint8_t *dst, uint32_t maxlen);
  void cmdfifo_push(const uint8_t *src, uint32_t len);
  int select();
  void do_command_phase();
  void do_message_phase();
  void do_cmd();
  bool cdb_ready();
  void dma_ti_check();
  void do_dma();
  void do_nodma();
  void nodma_ti_dataout();
  void handle_ti();
  void run_cmd();
  bool cmd_is_valid(uint8_t cmd) const;
  void hard_reset();
  void bus_reset();
  void cancel_request();
  void req_continue();
  void transfer_data(uint32_t len);
  void command_complete(uint8_t st);
  /* disk target */
  int32_t disk_command(const uint8_t *cdb, uint32_t cdblen);
  int32_t check_condition(uint8_t key, uint8_t asc, uint8_t ascq);
  int32_t data_in(const uint8_t *src, uint32_t len, uint32_t alloc);
  uint32_t mode_page(uint8_t page, uint8_t pc, uint8_t *p);
  void block_read(uint64_t lba, uint8_t *dst);
  void block_write(uint64_t lba, const uint8_t *src);
};

#endif
