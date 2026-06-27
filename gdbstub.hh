#ifndef __GDBSTUB_HH__
#define __GDBSTUB_HH__
/* Minimal gdb remote-serial-protocol (RSP) stub for interp_mips.
 *
 * Enabled with `--gdb <port>`.  Boots at full speed while no gdb is attached
 * (a cheap non-blocking accept is polled every ~64k instructions); attaching
 * gdb (`target remote :PORT`) stops the target so you can inspect the live
 * IRIX kernel with its symbols:
 *
 *   mips-linux-gnu-gdb unix.clean
 *   (gdb) set endian big
 *   (gdb) set architecture mips:isa64
 *   (gdb) target remote :1234
 *   (gdb) p (proc_t)*curprocp     ... break, step, x/, etc.
 *
 * Software breakpoints are kept in the stub (a pc set checked each step) so the
 * guest's read-only kernel text is never patched.  Registers are described to
 * gdb via a target.xml served over qXfer, using the legacy MIPS regnum layout.
 */
#include <cstdint>
#include <string>
#include <unordered_set>

class state_t;

class gdb_stub {
public:
  explicit gdb_stub(int port);
  /* called from the main loop just before each execMips(); cheap when no gdb
   * is attached and no breakpoints are set. */
  void step_hook(state_t *s);
private:
  int listen_fd = -1;
  int conn_fd   = -1;
  bool stepping = false;
  uint64_t poll_ctr = 0;
  std::unordered_set<uint32_t> breakpoints;

  void try_accept();
  void command_loop(state_t *s, int sig);   /* talk to gdb until it resumes */
  bool ctrlc_pending();
  std::string recv_packet();
  void send_packet(const std::string &body);
  std::string read_registers(state_t *s);
  void write_registers(state_t *s, const std::string &hex);
  std::string read_memory(state_t *s, uint64_t addr, uint32_t len);
  bool write_memory(state_t *s, uint64_t addr, const std::string &hex);
  std::string stop_reply(int sig);
};
#endif
