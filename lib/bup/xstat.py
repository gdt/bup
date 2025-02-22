"""Enhanced stat operations for bup."""

from time import strftime
import os, sys, time
import stat as pystat

from bup import _helpers


def timespec_to_nsecs(ts):
    ts_s, ts_ns = ts
    return ts_s * 10**9 + ts_ns


def nsecs_to_timespec(ns):
    """Return (s, ns) where ns is always non-negative
    and t = s + ns / 10e8""" # metadata record rep
    ns = int(ns)
    return (ns // 10**9, ns % 10**9)


def nsecs_to_timeval(ns):
    """Return (s, us) where ns is always non-negative
    and t = s + us / 10e5"""
    ns = int(ns)
    return (ns // 10**9, (ns % 10**9) // 1000)


def fstime_floor_secs(ns):
    """Return largest integer not greater than ns / 10e8."""
    return int(ns) // 10**9;


def fstime_to_timespec(ns):
    return nsecs_to_timespec(ns)


def fstime_to_sec_bytes(fstime):
    (s, ns) = fstime_to_timespec(fstime)
    if(s < 0):
        s += 1
    if ns == 0:
        return b'%d' % s
    else:
        return b'%d.%09d' % (s, ns)

def utime(path, times):
    """Times must be provided as (atime_ns, mtime_ns)."""
    os.utime(path, ns=times)
def lutime(path, times):
    """Times must be provided as (atime_ns, mtime_ns)."""
    os.utime(path, ns=times, follow_symlinks=False)

_cygwin_sys = sys.platform.startswith('cygwin')

def _fix_cygwin_id(id):
    if id < 0:
        id += 0x100000000
        assert(id >= 0)
    return id


class stat_result:
    __slots__ = ('st_mode', 'st_ino', 'st_dev', 'st_nlink', 'st_uid', 'st_gid',
                 'st_rdev', 'st_size', 'st_atime', 'st_mtime', 'st_ctime')
    @staticmethod
    def from_xstat_rep(st):
        global _cygwin_sys
        result = stat_result()
        (result.st_mode,
         result.st_ino,
         result.st_dev,
         result.st_nlink,
         result.st_uid,
         result.st_gid,
         result.st_rdev,
         result.st_size,
         result.st_atime,
         result.st_mtime,
         result.st_ctime) = st
        # Inlined timespec_to_nsecs after profiling
        result.st_atime = result.st_atime[0] * 10**9 + result.st_atime[1]
        result.st_mtime = result.st_mtime[0] * 10**9 + result.st_mtime[1]
        result.st_ctime = result.st_ctime[0] * 10**9 + result.st_ctime[1]
        if _cygwin_sys:
            result.st_uid = _fix_cygwin_id(result.st_uid)
            result.st_gid = _fix_cygwin_id(result.st_gid)
        return result


def stat(path):
    return stat_result.from_xstat_rep(_helpers.stat(path))


def fstat(path):
    return stat_result.from_xstat_rep(_helpers.fstat(path))


def lstat(path):
    return stat_result.from_xstat_rep(_helpers.lstat(path))


def mode_str(mode):
    result = ''
    # FIXME: Other types?
    if pystat.S_ISREG(mode):
        result += '-'
    elif pystat.S_ISDIR(mode):
        result += 'd'
    elif pystat.S_ISCHR(mode):
        result += 'c'
    elif pystat.S_ISBLK(mode):
        result += 'b'
    elif pystat.S_ISFIFO(mode):
        result += 'p'
    elif pystat.S_ISLNK(mode):
        result += 'l'
    elif pystat.S_ISSOCK(mode):
        result += 's'
    else:
        result += '?'

    result += 'r' if (mode & pystat.S_IRUSR) else '-'
    result += 'w' if (mode & pystat.S_IWUSR) else '-'
    result += 'x' if (mode & pystat.S_IXUSR) else '-'
    result += 'r' if (mode & pystat.S_IRGRP) else '-'
    result += 'w' if (mode & pystat.S_IWGRP) else '-'
    result += 'x' if (mode & pystat.S_IXGRP) else '-'
    result += 'r' if (mode & pystat.S_IROTH) else '-'
    result += 'w' if (mode & pystat.S_IWOTH) else '-'
    result += 'x' if (mode & pystat.S_IXOTH) else '-'
    return result


def classification_str(mode, include_exec):
    if pystat.S_ISREG(mode):
        if include_exec \
           and (pystat.S_IMODE(mode) \
                & (pystat.S_IXUSR | pystat.S_IXGRP | pystat.S_IXOTH)):
            return '*'
        else:
            return ''
    elif pystat.S_ISDIR(mode):
        return '/'
    elif pystat.S_ISLNK(mode):
        return '@'
    elif pystat.S_ISFIFO(mode):
        return '|'
    elif pystat.S_ISSOCK(mode):
        return '='
    else:
        return ''


def local_time_str(t):
    if t is None:
        return None
    return strftime('%Y-%m-%d %H:%M', time.localtime(fstime_floor_secs(t)))
