# SPDX-License-Identifier: MIT
"""UTF-16 editor changes and Mac child-process memory samples (all in bytes)."""

import ctypes

# Local Mac SDK sys/resource.h, rusage_info_v4. RSS and physical footprint
# are distinct; the lifetime maximum below applies to footprint, not RSS.
_USAGE_FIELDS = """
user_time system_time pkg_idle_wkups interrupt_wkups pageins wired_size
resident_size phys_footprint proc_start_abstime proc_exit_abstime child_user_time
child_system_time child_pkg_idle_wkups child_interrupt_wkups child_pageins
child_elapsed_abstime diskio_bytesread diskio_byteswritten cpu_time_qos_default
cpu_time_qos_maintenance cpu_time_qos_background cpu_time_qos_utility
cpu_time_qos_legacy cpu_time_qos_user_initiated cpu_time_qos_user_interactive
billed_system_time serviced_system_time logical_writes lifetime_max_phys_footprint
instructions cycles billed_energy serviced_energy interval_max_phys_footprint
runnable_time
""".split()


class Usage(ctypes.Structure):
    _fields_ = [("uuid", ctypes.c_uint8 * 16)] + [
        (name, ctypes.c_uint64) for name in _USAGE_FIELDS
    ]


_lib = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
_lib.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
_lib.proc_pid_rusage.restype = ctypes.c_int


def sample(pid):
    value = Usage()
    result = _lib.proc_pid_rusage(pid, 4, ctypes.byref(value))
    if result:
        return {"available": False, "errno": ctypes.get_errno()}
    return {
        "available": True,
        "rssBytes": value.resident_size,
        "physicalFootprintBytes": value.phys_footprint,
        "lifetimePeakPhysicalFootprintBytes": value.lifetime_max_phys_footprint,
        "processStartAbstime": value.proc_start_abstime,
    }


def position(text, offset, encoding="utf-16"):
    start = text.rfind("\n", 0, offset) + 1
    line = text[start:offset]
    width = (
        len(line.encode("utf-8"))
        if encoding == "utf-8"
        else len(line.encode("utf-16-le")) // 2
    )
    return {"line": text.count("\n", 0, offset), "character": width}


def range_change(uri, version, old, new, encoding):
    start = 0
    while start < min(len(old), len(new)) and old[start] == new[start]:
        start += 1
    suffix = 0
    while (
        suffix < min(len(old) - start, len(new) - start)
        and old[len(old) - 1 - suffix] == new[len(new) - 1 - suffix]
    ):
        suffix += 1
    # LSP positions cannot address the middle of a CRLF line ending.
    while start > 0 and (
        old[start - 1 : start + 1] == "\r\n" or new[start - 1 : start + 1] == "\r\n"
    ):
        start -= 1
    while suffix > 0 and (
        old[len(old) - suffix - 1 : len(old) - suffix + 1] == "\r\n"
        or new[len(new) - suffix - 1 : len(new) - suffix + 1] == "\r\n"
    ):
        suffix -= 1
    return {
        "method": "textDocument/didChange",
        "params": {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [
                {
                    "range": {
                        "start": position(old, start, encoding),
                        "end": position(old, len(old) - suffix, encoding),
                    },
                    "text": new[start : len(new) - suffix],
                }
            ],
        },
    }
