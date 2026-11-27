/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>

#include <fcntl.h>

#if REX_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif  // REX_PLATFORM_MAC

#if REX_PLATFORM_LINUX
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

// macOS off_t is 64-bit with no *64 large-file variants; Linux keeps the
// explicit *64 forms for legacy 32-bit off_t distributions.
#if defined(__APPLE__)
#define rex_mmap64 mmap
#define rex_ftruncate64 ftruncate
#else
#define rex_mmap64 mmap64
#define rex_ftruncate64 ftruncate64
#endif

#if REX_PLATFORM_ANDROID
#include <string.h>

#include <dlfcn.h>
#include <sys/ioctl.h>

#include <linux/ashmem.h>

// TODO(tomc): Android or maybe na. idk
// #include "xenia/base/main_android.h"
#endif

namespace rex {
namespace memory {

// Convert a filesystem path to a valid shm_open name (must start with /, no other slashes).
// macOS enforces a 31-character total limit, so long names are folded from the full path rather
// than only the filename. This keeps equal filenames in different directories distinct.
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/') {
      c = '_';
    }
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
#if REX_PLATFORM_MAC
  if (name.size() > 30) {
    const std::size_t h = std::hash<std::string>{}(name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    name = hash_buf;
  }
#endif
  return name;
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ = reinterpret_cast<decltype(android_ASharedMemory_create_)>(
          dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  return getpagesize();
}
size_t allocation_granularity() {
  return page_size();
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

bool IsWritableExecutableMemorySupported() {
#if REX_PLATFORM_MAC
  // macOS enforces W^X on Apple Silicon. Shared file mappings cannot be both
  // writable and executable. The code cache must use separate RW and RX views.
  return false;
#else
  return true;
#endif
}

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

// Parse a line from /proc/self/maps into a LinuxMapEntry
static bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3)
    return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

// PROCMAP_QUERY (Linux 6.11+): one ioctl on /proc/self/maps answers "which mapping holds this address" instead of
// reading and parsing the whole file, which for the guest's arena is thousands of lines on every look up.
//
// Battlefield 1943 Reborn, 17 Nov 2026: the ioctl's number and struct are the kernel's fixed UAPI, so they are
// written here rather than taken from <linux/fs.h>. With the header's `#ifdef PROCMAP_QUERY` the fast path was
// compiled out of every release build: they are made in an Ubuntu 22.04 root (kernel headers 5.15, for glibc 2.35)
// and Ubuntu 24.04's headers (6.8) lack it as well. Kernels older than 6.11 answer ENOTTY; the first such answer
// switches to reading the file for the rest of the run. REX_PROCMAP_QUERY=0 forces the file path (for tests).
struct RexProcmapQuery {
  uint64_t size;
  uint64_t query_flags;
  uint64_t query_addr;
  uint64_t vma_start;
  uint64_t vma_end;
  uint64_t vma_flags;
  uint64_t vma_page_size;
  uint64_t vma_offset;
  uint64_t inode;
  uint32_t dev_major;
  uint32_t dev_minor;
  uint32_t vma_name_size;
  uint32_t build_id_size;
  uint64_t vma_name_addr;
  uint64_t build_id_addr;
};
static_assert(sizeof(RexProcmapQuery) == 104, "struct procmap_query is 104 bytes in Linux 6.11");
// _IOWR('f', 17, struct procmap_query)
constexpr unsigned long kRexProcmapQuery =
    (3ul << 30) | (sizeof(RexProcmapQuery) << 16) | (static_cast<unsigned long>('f') << 8) | 17ul;
static_assert(kRexProcmapQuery == 0xC0686611ul, "PROCMAP_QUERY's ioctl number");
constexpr uint64_t kRexProcmapVmaReadable = 0x01;
constexpr uint64_t kRexProcmapVmaWritable = 0x02;
constexpr uint64_t kRexProcmapVmaExecutable = 0x04;
constexpr uint64_t kRexProcmapVmaShared = 0x08;
#ifdef PROCMAP_QUERY
// Headers that have it must agree with the copy above.
static_assert(sizeof(struct procmap_query) == sizeof(RexProcmapQuery), "procmap_query size");
static_assert(PROCMAP_QUERY == kRexProcmapQuery, "PROCMAP_QUERY number");
static_assert(PROCMAP_QUERY_VMA_READABLE == kRexProcmapVmaReadable &&
                  PROCMAP_QUERY_VMA_WRITABLE == kRexProcmapVmaWritable &&
                  PROCMAP_QUERY_VMA_EXECUTABLE == kRexProcmapVmaExecutable &&
                  PROCMAP_QUERY_VMA_SHARED == kRexProcmapVmaShared,
              "PROCMAP_QUERY flags");
#endif

// 1: the ioctl is used; 0: not tried yet; -1: off (kernel older than 6.11, or REX_PROCMAP_QUERY=0).
static std::atomic<int> procmap_query_state{0};
static std::atomic<int> procmap_query_errno{0};

static bool ProcmapQueryDisabledByEnvironment() {
  const char* value = std::getenv("REX_PROCMAP_QUERY");
  return value && value[0] == '0';
}

// One descriptor per thread, as pull request 28 had it: a plain int opened with open() only. This runs inside the
// write watch fault handler (a signal handler), so it must stay async signal safe: a thread_local with a destructor
// registers it through __cxa_thread_atexit, which takes the dynamic loader's lock and allocates, and a game hung
// that way on the Ally (17 Nov 2026). The descriptor of a thread that ends stays open (one per thread that ever
// asked; the guest's threads live for the whole run).
static thread_local int maps_fd = -2;  // -2: not opened yet; -1: open failed

// 1: answered (out_entry filled); 0: no mapping holds the address; -1: not answered, read the file.
static int ProcmapQueryLookup(uintptr_t addr, LinuxMapEntry& out_entry) {
  int state = procmap_query_state.load(std::memory_order_relaxed);
  if (state < 0) return -1;
  if (state == 0 && ProcmapQueryDisabledByEnvironment()) {
    procmap_query_state.store(-1, std::memory_order_relaxed);
    return -1;
  }
  if (maps_fd == -2) maps_fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (maps_fd < 0) return -1;
  RexProcmapQuery query{};
  query.size = sizeof(query);
  query.query_addr = addr;
  if (ioctl(maps_fd, kRexProcmapQuery, &query) == 0) {
    if (state == 0) procmap_query_state.store(1, std::memory_order_relaxed);
    out_entry.start = static_cast<uintptr_t>(query.vma_start);
    out_entry.end = static_cast<uintptr_t>(query.vma_end);
    out_entry.perms[0] = query.vma_flags & kRexProcmapVmaReadable ? 'r' : '-';
    out_entry.perms[1] = query.vma_flags & kRexProcmapVmaWritable ? 'w' : '-';
    out_entry.perms[2] = query.vma_flags & kRexProcmapVmaExecutable ? 'x' : '-';
    out_entry.perms[3] = query.vma_flags & kRexProcmapVmaShared ? 's' : 'p';
    out_entry.perms[4] = '\0';
    return 1;
  }
  const int error = errno;
  if (error == ENOENT) return 0;
  if (error == ENOTTY || error == EINVAL || error == EOPNOTSUPP) {
    // The kernel has no PROCMAP_QUERY: do not ask again.
    procmap_query_errno.store(error, std::memory_order_relaxed);
    procmap_query_state.store(-1, std::memory_order_relaxed);
  }
  return -1;
}

// Find the mapping entry in /proc/self/maps that contains the given address
static bool FindEntryForAddress(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  const int answered = ProcmapQueryLookup(addr, out_entry);
  if (answered >= 0) return answered == 1;
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (addr >= e.start && addr < e.end) {
      out_entry = e;
      return true;
    }
  }
  return false;
}

// Check if [base, base+length) is fully covered by existing mappings (no gaps)
static bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0)
    return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;
  if (end < begin) {  // overflow check
    return false;
  }

  // msync with MS_ASYNC fails with ENOMEM where any page of the range is not mapped and does nothing else. It
  // replaces reading /proc/self/maps, which took about 0.2 ms on every commit: a game committing 300 pages one
  // by one stalled for 80 ms (King Kong, ROG Ally).
  const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
  const uintptr_t aligned = begin & ~(page - 1);
  return msync(reinterpret_cast<void*>(aligned), end - aligned, MS_ASYNC) == 0;
}

// Convert /proc/self/maps permission chars to PageAccess
static PageAccess PermsToPageAccess(const char perms[5]) {
  const bool r = perms[0] == 'r';
  const bool w = perms[1] == 'w';
  const bool x = perms[2] == 'x';

  if (!r && !w && !x)
    return PageAccess::kNoAccess;
  if (x)
    return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

}  // namespace

// Called once at start (Memory::Initialize): tries PROCMAP_QUERY on a mapping that surely exists and logs which way
// page protection is read for this run, so a log shows whether the fast path is in use.
void LogHostPageQueryPath() {
  static int probe_target = 0;
  LinuxMapEntry entry;
  const int answered = ProcmapQueryLookup(reinterpret_cast<uintptr_t>(&probe_target), entry);
  if (answered == 1) {
    REXSYS_INFO("Page queries: PROCMAP_QUERY (Linux 6.11+), one ioctl per look up");
  } else if (ProcmapQueryDisabledByEnvironment()) {
    REXSYS_INFO("Page queries: reading /proc/self/maps (REX_PROCMAP_QUERY=0)");
  } else {
    const int error = procmap_query_errno.load(std::memory_order_relaxed);
    REXSYS_INFO("Page queries: reading /proc/self/maps (no PROCMAP_QUERY on this kernel: {})",
                error ? strerror(error) : "probe failed");
  }
}
#endif  // REX_PLATFORM_LINUX

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  // Emulates Windows VirtualAlloc behavior:
  // - Reserve: create PROT_NONE mapping to hold address space
  // - Commit on existing reservation: mprotect to enable access (EEXIST path)
  // - New allocation: mmap with MAP_FIXED_NOREPLACE (never silently replace)
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  // Determine initial protection based on allocation type
  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = static_cast<int>(prot_requested);
      break;
  }

    // On macOS, MAP_FIXED_NOREPLACE is unavailable. kCommit on a pre-reserved
    // range uses mprotect to avoid clobbering the existing reservation. Do not
    // widen sub-host-page requests here - higher-level guest heaps must reconcile
    // all guest permissions sharing a host page first.
#if REX_PLATFORM_MAC
  if (base_address != nullptr && allocation_type == AllocationType::kCommit) {
    const size_t host_page = page_size();
    const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
    if ((address % host_page) != 0 || (length % host_page) != 0) {
      return nullptr;
    }
    if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
      return base_address;
    }
    return nullptr;
  }
#endif

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if REX_PLATFORM_MAC
  if (access == PageAccess::kExecuteReadWrite || access == PageAccess::kExecuteReadOnly) {
    flags |= MAP_JIT;
  }
  if (base_address) {
    flags |= MAP_FIXED;
  }
#elif defined(MAP_FIXED_NOREPLACE)
  if (base_address) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#else
  if (base_address) {
    flags |= MAP_FIXED;
  }
#endif

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  if (result != MAP_FAILED) {
    return result;
  }
#if defined(MAP_FIXED_NOREPLACE) && REX_PLATFORM_LINUX
  // Handle EEXIST: address already has a mapping (e.g., from prior Reserve)
  // This is the "commit on existing reservation" path
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    // Verify the entire range is mapped before using mprotect
    if (IsRangeFullyMapped(base_address, length)) {
      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
        return base_address;
      }
    }
  }
#endif

  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      // Decommit: remove access first, then release physical pages
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        return false;
      }
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
      return munmap(base_address, length) == 0;
    }
    default:
      // how we get here? :(
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }

#if REX_PLATFORM_MAC
  // mprotect doesn't report the previous protection. Query the Mach region
  // before changing it so this matches VirtualProtect's out parameter.
  if (out_old_access) {
    size_t old_region_length = 0;
    QueryProtect(base_address, old_region_length, *out_old_access);
  }
#elif REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it
  if (out_old_access) {
    LinuxMapEntry e;
    if (FindEntryForAddress(base_address, e)) {
      *out_old_access = PermsToPageAccess(e.perms);
    }
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
  int ret = mprotect(base_address, length, prot);
  if (ret != 0) {
    REXSYS_ERROR("mprotect({}, 0x{:X}, {}) failed: {} ({})", base_address, length, prot,
                 strerror(errno), errno);
  }
  return ret == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if REX_PLATFORM_MAC
  mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name;

  kern_return_t kr =
      mach_vm_region(mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
  if (kr != KERN_SUCCESS) {
    return false;
  }
  if (address > reinterpret_cast<mach_vm_address_t>(base_address)) {
    return false;
  }

  length = static_cast<size_t>((address + region_size) -
                               reinterpret_cast<mach_vm_address_t>(base_address));

  if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
      (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadWrite;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) ==
             (VM_PROT_READ | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadOnly;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE)) {
    access_out = PageAccess::kReadWrite;
  } else if (info.protection & VM_PROT_READ) {
    access_out = PageAccess::kReadOnly;
  } else {
    access_out = PageAccess::kNoAccess;
  }
  return true;
#elif !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  LinuxMapEntry e;
  if (!FindEntryForAddress(base_address, e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);

  return true;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
#if REX_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? static_cast<FileMappingHandle>(sharedmem_fd)
                             : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), rex::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ashmem_fd);
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (rex_ftruncate64(ret, static_cast<off_t>(length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  // Only the descriptor is used from here on, so remove the name now: the
  // memory then goes away with the process however it ends. Otherwise a
  // crash or a kill leaves the whole guest address space in /dev/shm (RAM)
  // until a reboot, and once that fills the next run dies with SIGBUS.
  shm_unlink(full_path.c_str());
  return static_cast<FileMappingHandle>(ret);
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
#if !REX_PLATFORM_ANDROID
  auto full_path = MakeShmName(path);
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // file_offset must be page-aligned
  const size_t page = page_size();
  if (file_offset % page != 0) {
    return nullptr;
  }

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  void* result = rex_mmap64(base_address, length, prot, flags, static_cast<int>(handle),
                            static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Verify we got the address we asked for
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
