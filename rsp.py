#!/usr/bin/env python3
"""Tiny gdb-RSP client for poking interp_mips's stub."""
import socket, sys, struct

class RSP:
    def __init__(self, host="127.0.0.1", port=1234):
        self.s = socket.create_connection((host, port))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(20)
    def _send(self, body):
        ck = sum(body.encode()) & 0xff
        self.s.sendall(b"$" + body.encode() + b"#%02x" % ck)
        # read ack
        a = self.s.recv(1)
    def _recv(self):
        buf = b""
        # skip to '$'
        while True:
            c = self.s.recv(1)
            if not c: raise EOFError
            if c == b'$': break
        while True:
            c = self.s.recv(1)
            if c == b'#': break
            buf += c
        self.s.recv(2)  # checksum
        self.s.sendall(b"+")
        return buf.decode()
    def cmd(self, body):
        self._send(body); return self._recv()
    def regs(self):
        h = self.cmd("g")
        # 72 regs: 38 64-bit (gpr0-31,status,lo,hi,badv,cause,pc) + 32 fp 64-bit + 2 32-bit
        vals = {}
        names = [f"r{i}" for i in range(32)] + ["status","lo","hi","badvaddr","cause","pc"]
        off = 0
        for n in names:
            vals[n] = int(h[off:off+16], 16); off += 16
        return vals
    def rdmem(self, addr, length):
        r = self.cmd("m%x,%x" % (addr & 0xffffffffffffffff, length))
        if r.startswith("E"): return None
        return bytes.fromhex(r)
    def rd32(self, addr):
        b = self.rdmem(addr, 4)
        return struct.unpack(">I", b)[0] if b else None
    def setbp(self, addr): return self.cmd("Z0,%x,4" % addr)
    def delbp(self, addr): return self.cmd("z0,%x,4" % addr)
    def cont(self):
        self._send("c"); return self._recv()   # blocks until stop
    def step(self):
        self._send("s"); return self._recv()

if __name__ == "__main__":
    r = RSP()
    print("qSupported:", r.cmd("qSupported"))
    print("? :", r.cmd("?"))
    rg = r.regs()
    print("pc=%016x ra=%016x sp=%016x status=%016x cause=%016x" % (
        rg["pc"], rg["r31"], rg["r29"], rg["status"], rg["cause"]))
    # read 16 bytes of kernel text at pc
    pc = rg["pc"]
    m = r.rdmem(pc, 16)
    print("mem@pc:", m.hex() if m else None)
