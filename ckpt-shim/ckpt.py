"""Parser for the interp_mips checkpoint format (saveState.cc cp_header).

Layout is __attribute__((packed)) little-endian on the HOST that wrote it:
  u64 magic; i64 pc; i64 gpr[32]; i64 hi; i64 lo;
  u32 cpr0[32]; u64 cpr0_64[32]; u64 cpr1[32]; u32 fcr1[5];
  cp_tlb tlb[48]  = { u64 entry_hi; u64 entry_lo0; u64 entry_lo1; u32 page_mask; u32 pad }
  u64 icnt; u32 num_pages;
then num_pages * { u32 va; u8 data[4096] }.
"""
import struct

CKPT_MAGIC = 0x6d697073f5f5d005
NTLB = 48


class Checkpoint(object):
    def __init__(self, path):
        with open(path, "rb") as f:
            blob = f.read()
        off = 0

        def rd(fmt):
            nonlocal off
            sz = struct.calcsize(fmt)
            v = struct.unpack_from(fmt, blob, off)
            off += sz
            return v

        (self.magic,) = rd("<Q")
        if self.magic != CKPT_MAGIC:
            raise ValueError("bad magic %016x (not an interp_mips checkpoint)" % self.magic)
        (self.pc,) = rd("<q")
        self.gpr = list(rd("<32q"))
        (self.hi,) = rd("<q")
        (self.lo,) = rd("<q")
        self.cpr0 = list(rd("<32I"))
        self.cpr0_64 = list(rd("<32Q"))
        self.cpr1 = list(rd("<32Q"))
        self.fcr1 = list(rd("<5I"))
        self.tlb = []
        for _ in range(NTLB):
            hi, lo0, lo1, pm, _pad = rd("<QQQII")
            self.tlb.append({"hi": hi, "lo0": lo0, "lo1": lo1, "mask": pm})
        (self.icnt,) = rd("<Q")
        (self.num_pages,) = rd("<I")

        self.pages = []
        for _ in range(self.num_pages):
            (va,) = struct.unpack_from("<I", blob, off)
            off += 4
            self.pages.append((va, blob[off:off + 4096]))
            off += 4096

    # CP0 indices we care about by name (MIPS III)
    STATUS, CAUSE, EPC = 12, 13, 14

    def summary(self):
        return ("pc=%016x icnt=%d pages=%d Status=%08x EPC=%016x  "
                "tlb_valid=%d" % (
                    self.pc & 0xFFFFFFFFFFFFFFFF, self.icnt, self.num_pages,
                    self.cpr0[self.STATUS], self.cpr0_64[self.EPC],
                    sum(1 for t in self.tlb if t["lo0"] & 2 or t["lo1"] & 2)))
