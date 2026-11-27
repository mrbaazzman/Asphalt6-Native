"""What this machine can afford: CPU count and free memory, without dependencies."""
import ctypes
import os


def cpu_count() -> int:
    try:
        return len(os.sched_getaffinity(0))
    except AttributeError:
        return os.cpu_count() or 4


def memory_gb() -> tuple:
    try:
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong),
                        ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong),
                        ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong),
                        ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        stat = MEMORYSTATUSEX()
        stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat)):
            return (stat.ullTotalPhys / 1073741824.0,
                    stat.ullAvailPhys / 1073741824.0)
    except Exception:
        pass
    return (8.0, 8.0)


def jobs(per_job_gb: float = 1.0, hard_max: int = 8) -> int:
    cpus = cpu_count()
    _total, available = memory_gb()
    by_memory = int(available // per_job_gb)
    return max(1, min(cpus, by_memory, hard_max))


def describe(per_job_gb: float = 1.0) -> str:
    total, available = memory_gb()
    return f"{cpu_count()} CPUs, {total:.0f} GiB RAM ({available:.0f} GiB free) -> {jobs(per_job_gb)} job(s)"
