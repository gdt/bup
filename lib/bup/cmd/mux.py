
import os, subprocess, sys

from bup import options
from bup.helpers import debug1, debug2, mux
from bup.io import byte_stream


optspec = """
bup mux command [arguments...]
--
"""

def main(argv):
    # Give the subcommand exclusive access to stdin.
    orig_stdin = os.dup(0)
    devnull = os.open(os.devnull, os.O_RDONLY)
    os.dup2(devnull, 0)
    os.close(devnull)

    o = options.Options(optspec)
    opt, flags, extra = o.parse_bytes(argv[1:])
    if len(extra) < 1:
        o.fatal('command is required')

    subcmd = extra

    debug2('bup mux: starting %r\n' % (extra,))

    outr, outw = os.pipe()
    errr, errw = os.pipe()
    def close_fds():
        os.close(outr)
        os.close(errr)

    p = subprocess.Popen(subcmd, stdin=orig_stdin, stdout=outw, stderr=errw,
                         close_fds=False, preexec_fn=close_fds)
    os.close(outw)
    os.close(errw)
    sys.stdout.flush()
    out = byte_stream(sys.stdout)
    out.write(b'BUPMUX')
    out.flush()
    mux(p, out.fileno(), outr, errr)
    os.close(outr)
    os.close(errr)
    prv = p.wait()

    if prv:
        debug1('%s exited with code %d\n' % (extra[0], prv))

    debug1('bup mux: done\n')

    sys.exit(prv)
