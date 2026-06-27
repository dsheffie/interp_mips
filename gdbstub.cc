#include "gdbstub.hh"
#include "interpret.hh"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <csignal>

/* ---- register layout (legacy MIPS regnums; see target.xml below) ---------
 * 0-31 gpr, 32 status, 33 lo, 34 hi, 35 badvaddr, 36 cause, 37 pc,
 * 38-69 fp0..fp31, 70 fcsr, 71 fir.  All 64-bit except fcsr/fir (32-bit). */

static std::string build_target_xml() {
  std::string x =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "  <architecture>mips:isa64</architecture>\n"
    "  <feature name=\"org.gnu.gdb.mips.cpu\">\n";
  for(int i = 0; i < 32; i++) {
    char b[96];
    snprintf(b, sizeof(b), "    <reg name=\"r%d\" bitsize=\"64\" regnum=\"%d\"/>\n", i, i);
    x += b;
  }
  x += "    <reg name=\"lo\" bitsize=\"64\" regnum=\"33\"/>\n"
       "    <reg name=\"hi\" bitsize=\"64\" regnum=\"34\"/>\n"
       "    <reg name=\"pc\" bitsize=\"64\" regnum=\"37\"/>\n"
       "  </feature>\n"
       "  <feature name=\"org.gnu.gdb.mips.cp0\">\n"
       "    <reg name=\"status\" bitsize=\"64\" regnum=\"32\"/>\n"
       "    <reg name=\"badvaddr\" bitsize=\"64\" regnum=\"35\"/>\n"
       "    <reg name=\"cause\" bitsize=\"64\" regnum=\"36\"/>\n"
       "  </feature>\n"
       "  <feature name=\"org.gnu.gdb.mips.fpu\">\n";
  for(int i = 0; i < 32; i++) {
    char b[112];
    snprintf(b, sizeof(b), "    <reg name=\"f%d\" bitsize=\"64\" type=\"ieee_double\" regnum=\"%d\"/>\n", i, 38 + i);
    x += b;
  }
  x += "    <reg name=\"fcsr\" bitsize=\"32\" group=\"float\" regnum=\"70\"/>\n"
       "    <reg name=\"fir\" bitsize=\"32\" group=\"float\" regnum=\"71\"/>\n"
       "  </feature>\n"
       "</target>\n";
  return x;
}
static const std::string g_target_xml = build_target_xml();

static int hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}
static void put_hex_be(std::string &s, uint64_t v, int nbytes) {
  /* emit a register value in target (big-endian) byte order, MSB first */
  for(int i = nbytes - 1; i >= 0; i--) {
    char b[3]; snprintf(b, sizeof(b), "%02x", (unsigned)((v >> (8 * i)) & 0xff)); s += b;
  }
}

gdb_stub::gdb_stub(int port) {
  signal(SIGPIPE, SIG_IGN);   /* a gdb client disconnect must never kill the ISS */
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((uint16_t)port);
  if(bind(listen_fd, (sockaddr*)&a, sizeof(a)) < 0 || listen(listen_fd, 1) < 0) {
    perror("gdbstub bind/listen"); close(listen_fd); listen_fd = -1; return;
  }
  fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);
  fprintf(stderr, "[gdb] listening on 127.0.0.1:%d (attach with: target remote :%d)\n", port, port);
}

void gdb_stub::try_accept() {
  int fd = accept(listen_fd, nullptr, nullptr);
  if(fd < 0) return;
  int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  conn_fd = fd;
  fprintf(stderr, "[gdb] client attached\n");
}

void gdb_stub::step_hook(state_t *s) {
  if(conn_fd < 0) {                                  /* unattached: full speed */
    if((++poll_ctr & 0xffffu) == 0) try_accept();
    if(conn_fd < 0) return;
    command_loop(s, -1);                             /* just attached: wait for gdb, no proactive stop */
    return;
  }
  if(stepping) { stepping = false; command_loop(s, 5); return; }
  if(!breakpoints.empty() && breakpoints.count((uint32_t)s->pc))
    { command_loop(s, 5); return; }
  if((++poll_ctr & 0xffffu) == 0 && ctrlc_pending()) command_loop(s, 2);  /* SIGINT */
}

bool gdb_stub::ctrlc_pending() {
  char c; ssize_t n = recv(conn_fd, &c, 1, MSG_DONTWAIT);
  if(n == 0) { close(conn_fd); conn_fd = -1; return false; }  /* client vanished mid-run: recover */
  return (n == 1 && c == 0x03);
}

/* read one '$...#xx' packet body, ACK it; returns "" on EOF/detach. Skips
 * leading +/-/0x03 bytes. */
std::string gdb_stub::recv_packet() {
  char c;
  for(;;) {
    ssize_t n = recv(conn_fd, &c, 1, 0);
    if(n <= 0) { close(conn_fd); conn_fd = -1; return ""; }
    if(c == '$') break;
    /* ignore +,-,0x03 between packets */
  }
  std::string body;
  for(;;) {
    ssize_t n = recv(conn_fd, &c, 1, 0);
    if(n <= 0) { close(conn_fd); conn_fd = -1; return ""; }
    if(c == '#') break;
    body += c;
  }
  char ck[2];
  if(recv(conn_fd, &ck[0], 1, 0) <= 0 || recv(conn_fd, &ck[1], 1, 0) <= 0)
    { close(conn_fd); conn_fd = -1; return ""; }
  const char ack = '+';
  (void)!write(conn_fd, &ack, 1);
  return body;
}

void gdb_stub::send_packet(const std::string &body) {
  uint8_t sum = 0; for(char c : body) sum += (uint8_t)c;
  std::string p = "$"; p += body; char t[4]; snprintf(t, sizeof(t), "#%02x", sum); p += t;
  if(write(conn_fd, p.data(), p.size()) != (ssize_t)p.size()) { close(conn_fd); conn_fd = -1; return; }
  char ack;                                          /* swallow gdb's +/- */
  if(recv(conn_fd, &ack, 1, 0) <= 0) { close(conn_fd); conn_fd = -1; }
}

std::string gdb_stub::stop_reply(int sig) {
  char b[8]; snprintf(b, sizeof(b), "S%02x", sig & 0xff); return b;
}

std::string gdb_stub::read_registers(state_t *s) {
  std::string r;
  for(int i = 0; i < 32; i++) put_hex_be(r, (uint64_t)s->gpr[i], 8);  /* 0-31 */
  put_hex_be(r, (uint64_t)(int32_t)s->cpr0[CPR0_SR], 8);              /* 32 status */
  put_hex_be(r, (uint64_t)s->lo, 8);                                 /* 33 lo */
  put_hex_be(r, (uint64_t)s->hi, 8);                                 /* 34 hi */
  put_hex_be(r, (uint64_t)(int32_t)s->cpr0[CPR0_BADVADDR], 8);       /* 35 badvaddr */
  put_hex_be(r, (uint64_t)(int32_t)s->cpr0[CPR0_CAUSE], 8);          /* 36 cause */
  put_hex_be(r, (uint64_t)s->pc, 8);                                 /* 37 pc */
  for(int i = 0; i < 32; i++) put_hex_be(r, s->cpr1[i], 8);          /* 38-69 fp */
  put_hex_be(r, 0, 4);                                               /* 70 fcsr */
  put_hex_be(r, 0, 4);                                               /* 71 fir */
  return r;
}

void gdb_stub::write_registers(state_t *s, const std::string &h) {
  auto rd = [&](size_t off, int nbytes) -> uint64_t {
    uint64_t v = 0;
    for(int i = 0; i < nbytes; i++)                  /* big-endian: first hex byte is MSB */
      v = (v << 8) | ((hexval(h[off + 2*i]) << 4) | hexval(h[off + 2*i + 1]));
    return v;
  };
  size_t o = 0;
  for(int i = 0; i < 32; i++) { s->gpr[i] = (int64_t)rd(o, 8); o += 16; }
  s->cpr0[CPR0_SR] = (uint32_t)rd(o, 8); o += 16;
  s->lo = (int64_t)rd(o, 8); o += 16;
  s->hi = (int64_t)rd(o, 8); o += 16;
  o += 16;                                            /* badvaddr (read-only) */
  o += 16;                                            /* cause (read-only) */
  s->pc = (int64_t)rd(o, 8); o += 16;
  for(int i = 0; i < 32; i++) { s->cpr1[i] = rd(o, 8); o += 16; }
}

std::string gdb_stub::read_memory(state_t *s, uint64_t addr, uint32_t len) {
  std::string r;
  for(uint32_t i = 0; i < len; i++) {
    uint8_t byte;
    if(!gdb_mem_read(s, addr + i, &byte, 1)) return "E03";
    char b[3]; snprintf(b, sizeof(b), "%02x", byte); r += b;
  }
  return r;
}

bool gdb_stub::write_memory(state_t *s, uint64_t addr, const std::string &h) {
  uint32_t len = h.size() / 2;
  for(uint32_t i = 0; i < len; i++) {
    uint8_t byte = (hexval(h[2*i]) << 4) | hexval(h[2*i + 1]);
    if(!gdb_mem_write(s, addr + i, &byte, 1)) return false;
  }
  return true;
}

void gdb_stub::command_loop(state_t *s, int sig) {
  if(sig >= 0) send_packet(stop_reply(sig));          /* report why we stopped */
  while(conn_fd >= 0) {
    std::string p = recv_packet();
    if(conn_fd < 0) return;                            /* detached */
    if(p.empty()) { send_packet(""); continue; }
    char c = p[0];
    if(c == 'q') {
      if(p.rfind("qSupported", 0) == 0)
        send_packet("PacketSize=4000;qXfer:features:read+");
      else if(p.rfind("qXfer:features:read:target.xml:", 0) == 0) {
        /* qXfer:features:read:target.xml:OFFSET,LENGTH */
        size_t comma = p.rfind(',');
        size_t colon = p.rfind(':', comma);
        unsigned long off = strtoul(p.c_str() + colon + 1, nullptr, 16);
        unsigned long ln  = strtoul(p.c_str() + comma + 1, nullptr, 16);
        if(off >= g_target_xml.size()) send_packet("l");
        else {
          size_t n = std::min((size_t)ln, g_target_xml.size() - off);
          std::string chunk = g_target_xml.substr(off, n);
          send_packet((off + n < g_target_xml.size() ? "m" : "l") + chunk);
        }
      }
      else if(p.rfind("qAttached", 0) == 0) send_packet("1");
      else if(p == "qC") send_packet("QC1");
      else if(p.rfind("qfThreadInfo", 0) == 0) send_packet("m1");
      else if(p.rfind("qsThreadInfo", 0) == 0) send_packet("l");
      else send_packet("");
    }
    else if(c == '?') send_packet(stop_reply(5));
    else if(c == 'g') send_packet(read_registers(s));
    else if(c == 'G') { write_registers(s, p.substr(1)); send_packet("OK"); }
    else if(c == 'm') {
      uint64_t addr; uint32_t len;
      sscanf(p.c_str() + 1, "%lx,%x", (unsigned long*)&addr, &len);
      send_packet(read_memory(s, addr, len));
    }
    else if(c == 'M') {
      uint64_t addr; uint32_t len; char data[8192];
      sscanf(p.c_str() + 1, "%lx,%x:%8191s", (unsigned long*)&addr, &len, data);
      const char *colon = strchr(p.c_str(), ':');
      send_packet(colon && write_memory(s, addr, colon + 1) ? "OK" : "E03");
    }
    else if(c == 'Z' && p[1] == '0') {
      uint64_t addr; sscanf(p.c_str() + 3, "%lx", (unsigned long*)&addr);
      breakpoints.insert((uint32_t)addr); send_packet("OK");
    }
    else if(c == 'z' && p[1] == '0') {
      uint64_t addr; sscanf(p.c_str() + 3, "%lx", (unsigned long*)&addr);
      breakpoints.erase((uint32_t)addr); send_packet("OK");
    }
    else if(c == 'c') return;                          /* continue */
    else if(c == 's') { stepping = true; return; }     /* single step */
    else if(c == 'H' || c == 'T') send_packet("OK");
    else if(c == 'D') { send_packet("OK"); close(conn_fd); conn_fd = -1; return; }
    else if(c == 'k') { send_packet("OK"); exit(0); }
    else send_packet("");                              /* unknown -> empty */
  }
}
