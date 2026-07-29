#!/usr/bin/env python3
"""Expect-style console driver for the interp_mips IRIX golden model.

Spawns interp_mips (disk-backed IRIX), watches the SCC console on stdout, and
feeds the login + a command script on stdin at the right boot stages (getty
flushes type-ahead, so we can't just pre-load the pipe).  The ASIDPC retire
trace (env ASIDPC=<file>) records {asid,pc} per userspace insn while the
workload runs -> that file IS the golden model for the be deep-trace align.

Usage:
  ASIDPC=/path/be.asidpc ./drive_irix.py --cmds cmds.txt [--user root] [--pass '']
  (--cmds file: one guest shell command per line, run after login)
"""
import argparse, os, pty, select, subprocess, sys, time, re

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--unix",  default="/home/dsheffie/code/chd-dumper/extracted/unix")
    ap.add_argument("--arcs",  default="/home/dsheffie/code/r9999/arcs/arcs_irix.bin")
    ap.add_argument("--disk",  default="irix65-working.img")
    ap.add_argument("--delta", default="/tmp/claude-1001/-home-dsheffie-code-r9999/3adf8984-63c0-4167-8f76-aed2a0de1bbb/scratchpad/golden.cow")
    ap.add_argument("--user",  default="root")
    ap.add_argument("--passwd", default="")
    ap.add_argument("--cmds",  required=True, help="file: one shell command per line")
    ap.add_argument("--boot-timeout", type=float, default=1200.0)
    ap.add_argument("--cmd-timeout",  type=float, default=7200.0)
    ap.add_argument("--logfile", default=None)
    args = ap.parse_args()

    cmds = [l.rstrip("\n") for l in open(args.cmds) if l.strip() and not l.startswith("#")]

    env = dict(os.environ)
    env.setdefault("DETTIME", "1")     # 2030 clock -> matches the reconfigured image

    # A pty makes IRIX getty/login behave (line discipline, echo) like a real console.
    master, slave = pty.openpty()
    cmd = ["./interp_mips", "-f", args.unix, "--arcs", args.arcs,
           "--disk", args.disk, "--disk-delta", args.delta]
    sys.stderr.write("[drive] launching: %s\n" % " ".join(cmd))
    proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave,
                            env=env, close_fds=True)
    os.close(slave)

    logf = open(args.logfile, "w") if args.logfile else None
    buf = ""     # rolling console text (last ~64k)

    def drain(timeout):
        nonlocal buf
        r, _, _ = select.select([master], [], [], timeout)
        if not r:
            return ""
        try:
            data = os.read(master, 4096)
        except OSError:
            return ""
        if not data:
            return ""
        txt = data.decode("latin-1", "replace")
        sys.stdout.write(txt); sys.stdout.flush()
        if logf: logf.write(txt); logf.flush()
        buf = (buf + txt)[-65536:]
        return txt

    def expect(patterns, timeout, label):
        """Wait until any regex in `patterns` appears in the rolling buffer."""
        nonlocal buf
        if isinstance(patterns, str): patterns = [patterns]
        t0 = time.time()
        while time.time() - t0 < timeout:
            if proc.poll() is not None:
                sys.stderr.write("[drive] interp EXITED (code %s) waiting for %s\n" % (proc.returncode, label))
                return None
            for p in patterns:
                m = re.search(p, buf)
                if m:
                    sys.stderr.write("[drive] matched %s: %r\n" % (label, m.group(0)))
                    return p
            drain(1.0)
        sys.stderr.write("[drive] TIMEOUT (%ss) waiting for %s\n" % (timeout, label))
        return None

    def send(s):
        sys.stderr.write("[drive] send: %r\n" % s)
        os.write(master, s.encode("latin-1"))

    # 1) login prompt
    if expect(r"login:", args.boot_timeout, "login") is None:
        proc.kill(); return 2
    buf = ""
    send(args.user + "\r")

    # 2) password
    hit = expect([r"[Pp]assword:", r"Login incorrect"], 60, "password-prompt")
    if hit is None:
        proc.kill(); return 3
    if "incorrect" in hit:
        sys.stderr.write("[drive] username rejected\n"); proc.kill(); return 4
    buf = ""
    send(args.passwd + "\r")

    # 3) the .login runs `tset` -> "TERM = (vt100) " terminal-type query that would
    #    otherwise eat our commands; accept the vt100 default with a bare Enter.
    hit = expect([r"Login incorrect", r"TERM\s*=\s*\("], 120, "term-or-fail")
    if hit and "incorrect" in hit:
        sys.stderr.write("[drive] LOGIN FAILED (wrong password?)\n"); proc.kill(); return 5
    if hit and "TERM" in hit:
        send("\r")
        time.sleep(1.0)

    # 4) confirm a live shell: kill tty echo, then emit a start-of-line marker so it
    #    matches as command OUTPUT, never as an input echo (the earlier false-match).
    buf = ""
    # quote-split marker: the input echo shows READY''_42x42, the command OUTPUT
    # shows READY_42x42 -> matching the un-quoted form hits only real output
    # (csh prints a numbered "IRIS N# " prompt before it, so no ^-anchor).
    send("echo READY''_42x42\r")
    hit = expect([r"Login incorrect", r"READY_42x42"], 120, "shell-ready")
    if hit is None or "incorrect" in hit:
        sys.stderr.write("[drive] LOGIN FAILED (no shell)\n"); proc.kill(); return 5
    sys.stderr.write("[drive] LOGIN OK, shell live\n")

    # 5) run the command script.  A line "@ARM" toggles the interp's ASIDPC trace
    #    (SIGUSR1) instead of running a guest command -> bracket the cc invocation
    #    so only the compile's userspace is recorded, not the whole boot.
    import signal as _sig
    for c in cmds:
        if c.strip() == "@ARM":
            sys.stderr.write("[drive] SIGUSR1 -> toggle ASIDPC arm\n")
            proc.send_signal(_sig.SIGUSR1)
            time.sleep(1.0)
            continue
        buf = ""
        send(c + "\r")
        # csh-safe fixed marker: NO $? ($?name is csh var-existence, not exit code).
        # quote-split so the input echo (__CMD''DONE__) differs from the OUTPUT.
        send("echo __CMD''DONE__\r")
        expect(r"__CMDDONE__", args.cmd_timeout, "cmd-done: %.40s" % c)

    # 5) clean shutdown of the driver (leave the guest; SIGUSR2 flushed already)
    send("sync\r"); time.sleep(2)
    sys.stderr.write("[drive] done; killing interp\n")
    proc.kill()
    try: proc.wait(timeout=10)
    except Exception: pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
