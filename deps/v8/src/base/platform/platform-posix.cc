// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Platform-specific code for POSIX goes here. This is not a platform on its
// own, but contains the parts which are the same across the POSIX platforms
// Linux, MacOS, FreeBSD, OpenBSD, NetBSD and QNX.

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>  // for pthread_set_name_np
#endif
#include <sched.h>  // for sched_yield
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/sysctl.h>  // for sysctl
#endif

#if defined(ANDROID) && !defined(V8_ANDROID_LOG_STDOUT)
#define LOG_TAG "v8"
#include <android/log.h>
#endif

#include <cmath>
#include <cstdlib>

#include "src/base/platform/platform-posix.h"

#include "src/base/lazy-instance.h"
#include "src/base/macros.h"
#include "src/base/platform/platform.h"
#include "src/base/platform/time.h"
#include "src/base/utils/random-number-generator.h"

#ifdef V8_FAST_TLS_SUPPORTED
#include <atomic>
#endif

#if V8_OS_MACOSX
#include <dlfcn.h>
#include <mach/mach.h>
#endif

#if V8_OS_LINUX
#include <sys/prctl.h>  // for prctl
#endif

#if defined(V8_OS_FUCHSIA)
#include <zircon/process.h>
#else
#include <sys/resource.h>
#endif

#if !defined(_AIX) && !defined(V8_OS_FUCHSIA)
#include <sys/syscall.h>
#endif

#if V8_OS_FREEBSD || V8_OS_MACOSX || V8_OS_OPENBSD || V8_OS_SOLARIS
#define MAP_ANONYMOUS MAP_ANON
#endif

#if defined(V8_OS_SOLARIS)
#if (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE > 2) || defined(__EXTENSIONS__)
extern "C" int madvise(caddr_t, size_t, int);
#else
extern int madvise(caddr_t, size_t, int);
#endif
#endif

#ifndef MADV_FREE
#define MADV_FREE MADV_DONTNEED
#endif

#if defined(V8_LIBC_GLIBC)
extern "C" void* __libc_stack_end;
#endif

namespace v8 {
namespace base {

namespace {

// 0 is never a valid thread id.
const pthread_t kNoThread = static_cast<pthread_t>(0);

bool g_hard_abort = false;

const char* g_gc_fake_mmap = nullptr;

DEFINE_LAZY_LEAKY_OBJECT_GETTER(RandomNumberGenerator,
                                GetPlatformRandomNumberGenerator)
static LazyMutex rng_mutex = LAZY_MUTEX_INITIALIZER;

#if !V8_OS_FUCHSIA
#if V8_OS_MACOSX
// kMmapFd is used to pass vm_alloc flags to tag the region with the user
// defined tag 255 This helps identify V8-allocated regions in memory analysis
// tools like vmmap(1).
const int kMmapFd = VM_MAKE_TAG(255);
#else   // !V8_OS_MACOSX
const int kMmapFd = -1;
#endif  // !V8_OS_MACOSX

#if defined(V8_TARGET_OS_MACOSX) && V8_HOST_ARCH_ARM64
// During snapshot generation in cross builds, sysconf() runs on the Intel
// host and returns host page size, while the snapshot needs to use the
// target page size.
constexpr int kAppleArmPageSize = 1 << 14;
#endif

const int kMmapFdOffset = 0;

// TODO(v8:10026): Add the right permission flag to make executable pages
// guarded.
int GetProtectionFromMemoryPermission(OS::MemoryPermission access) {
  switch (access) {
    case OS::MemoryPermission::kNoAccess:
    case OS::MemoryPermission::kNoAccessWillJitLater:
      return PROT_NONE;
    case OS::MemoryPermission::kRead:
      return PROT_READ;
    case OS::MemoryPermission::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case OS::MemoryPermission::kReadWriteExecute:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    case OS::MemoryPermission::kReadExecute:
      return PROT_READ | PROT_EXEC;
  }
  UNREACHABLE();
}

enum class PageType { kShared, kPrivate };

int GetFlagsForMemoryPermission(OS::MemoryPermission access,
                                PageType page_type) {
  int flags = MAP_ANONYMOUS;
  flags |= (page_type == PageType::kShared) ? MAP_SHARED : MAP_PRIVATE;
  if (access == OS::MemoryPermission::kNoAccess) {
#if !V8_OS_AIX && !V8_OS_FREEBSD && !V8_OS_QNX
    flags |= MAP_NORESERVE;
#endif  // !V8_OS_AIX && !V8_OS_FREEBSD && !V8_OS_QNX
#if V8_OS_QNX
    flags |= MAP_LAZY;
#endif  // V8_OS_QNX
  }
#if V8_HAS_PTHREAD_JIT_WRITE_PROTECT
  if (access == OS::MemoryPermission::kNoAccessWillJitLater) {
    flags |= MAP_JIT;
  }
#endif
  return flags;
}

void* Allocate(void* hint, size_t size, OS::MemoryPermission access,
               PageType page_type) {
  int prot = GetProtectionFromMemoryPermission(access);
  int flags = GetFlagsForMemoryPermission(access, page_type);
  void* result = mmap(hint, size, prot, flags, kMmapFd, kMmapFdOffset);
  if (result == MAP_FAILED) return nullptr;
#if ENABLE_HUGEPAGE
  if (result != nullptr && size >= kHugePageSize) {
    const uintptr_t huge_start =
        RoundUp(reinterpret_cast<uintptr_t>(result), kHugePageSize);
    const uintptr_t huge_end =
        RoundDown(reinterpret_cast<uintptr_t>(result) + size, kHugePageSize);
    if (huge_end > huge_start) {
      // Bail out in case the aligned addresses do not provide a block of at
      // least kHugePageSize size.
      madvise(reinterpret_cast<void*>(huge_start), huge_end - huge_start,
              MADV_HUGEPAGE);
    }
  }
#endif

  return result;
}

#endif  // !V8_OS_FUCHSIA

}  // namespace

#if V8_OS_LINUX || V8_OS_FREEBSD
#ifdef __arm__

bool OS::ArmUsingHardFloat() {
  // GCC versions 4.6 and above define __ARM_PCS or __ARM_PCS_VFP to specify
  // the Floating Point ABI used (PCS stands for Procedure Call Standard).
  // We use these as well as a couple of other defines to statically determine
  // what FP ABI used.
  // GCC versions 4.4 and below don't support hard-fp.
  // GCC versions 4.5 may support hard-fp without defining __ARM_PCS or
  // __ARM_PCS_VFP.

#define GCC_VERSION \
  (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#if GCC_VERSION >= 40600 && !defined(__clang__)
#if defined(__ARM_PCS_VFP)
  return true;
#else
  return false;
#endif

#elif GCC_VERSION < 40500 && !defined(__clang__)
  return false;

#else
#if defined(__ARM_PCS_VFP)
  return true;
#elif defined(__ARM_PCS) || defined(__SOFTFP__) || defined(__SOFTFP) || \
    !defined(__VFP_FP__)
  return false;
#else
#error \
    "Your version of compiler does not report the FP ABI compiled for."     \
       "Please report it on this issue"                                        \
       "http://code.google.com/p/v8/issues/detail?id=2140"

#endif
#endif
#undef GCC_VERSION
}

#endif  // def __arm__
#endif

void OS::Initialize(bool hard_abort, const char* const gc_fake_mmap) {
  g_hard_abort = hard_abort;
  g_gc_fake_mmap = gc_fake_mmap;
}

int OS::ActivationFrameAlignment() {
#if V8_TARGET_ARCH_ARM
  // On EABI ARM targets this is required for fp correctness in the
  // runtime system.
  return 8;
#elif V8_TARGET_ARCH_MIPS
  return 8;
#elif V8_TARGET_ARCH_S390
  return 8;
#else
  // Otherwise we just assume 16 byte alignment, i.e.:
  // - With gcc 4.4 the tree vectorization optimizer can generate code
  //   that requires 16 byte alignment such as movdqa on x86.
  // - Mac OS X, PPC and Solaris (64-bit) activation frames must
  //   be 16 byte-aligned;  see "Mac OS X ABI Function Call Guide"
  return 16;
#endif
}

// static
size_t OS::AllocatePageSize() {
#if defined(V8_TARGET_OS_MACOSX) && V8_HOST_ARCH_ARM64
  return kAppleArmPageSize;
#else
  static size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  return page_size;
#endif
}

// static
size_t OS::CommitPageSize() {
  // Commit and allocate page size are the same on posix.
  return OS::AllocatePageSize();
}

// static
void OS::SetRandomMmapSeed(int64_t seed) {
  if (seed) {
    MutexGuard guard(rng_mutex.Pointer());
    GetPlatformRandomNumberGenerator()->SetSeed(seed);
  }
}

// static
void* OS::GetRandomMmapAddr() {
  uintptr_t raw_addr;
  {
    MutexGuard guard(rng_mutex.Pointer());
    GetPlatformRandomNumberGenerator()->NextBytes(&raw_addr, sizeof(raw_addr));
  }
#if V8_HOST_ARCH_ARM64
#if defined(V8_TARGET_OS_MACOSX)
  DCHECK_EQ(1 << 14, AllocatePageSize());
#endif
  // Keep the address page-aligned, AArch64 supports 4K, 16K and 64K
  // configurations.
  raw_addr = RoundDown(raw_addr, AllocatePageSize());
#endif
#if defined(V8_USE_ADDRESS_SANITIZER) || defined(MEMORY_SANITIZER) || \
    defined(THREAD_SANITIZER) || defined(LEAK_SANITIZER)
  // If random hint addresses interfere with address ranges hard coded in
  // sanitizers, bad things happen. This address range is copied from TSAN
  // source but works with all tools.
  // See crbug.com/539863.
  raw_addr &= 0x007fffff0000ULL;
  raw_addr += 0x7e8000000000ULL;
#else
#if V8_TARGET_ARCH_X64 || V8_TARGET_ARCH_ARM64
  // Currently available CPUs have 48 bits of virtual addressing.  Truncate
  // the hint address to 46 bits to give the kernel a fighting chance of
  // fulfilling our placement request.
  raw_addr &= uint64_t{0x3FFFFFFFF000};
#elif V8_TARGET_ARCH_PPC64
#if V8_OS_AIX
  // AIX: 64 bits of virtual addressing, but we limit address range to:
  //   a) minimize Segment Lookaside Buffer (SLB) misses and
  raw_addr &= uint64_t{0x3FFFF000};
  // Use extra address space to isolate the mmap regions.
  raw_addr += uint64_t{0x400000000000};
#elif V8_TARGET_BIG_ENDIAN
  // Big-endian Linux: 42 bits of virtual addressing.
  raw_addr &= uint64_t{0x03FFFFFFF000};
#else
  // Little-endian Linux: 46 bits of virtual addressing.
  raw_addr &= uint64_t{0x3FFFFFFF0000};
#endif
#elif V8_TARGET_ARCH_S390X
  // Linux on Z uses bits 22-32 for Region Indexing, which translates to 42 bits
  // of virtual addressing.  Truncate to 40 bits to allow kernel chance to
  // fulfill request.
  raw_addr &= uint64_t{0xFFFFFFF000};
#elif V8_TARGET_ARCH_S390
  // 31 bits of virtual addressing.  Truncate to 29 bits to allow kernel chance
  // to fulfill request.
  raw_addr &= 0x1FFFF000;
#elif V8_TARGET_ARCH_MIPS64
  // 42 bits of virtual addressing. Truncate to 40 bits to allow kernel chance
  // to fulfill request.
  raw_addr &= uint64_t{0xFFFFFF0000};
#elif V8_TARGET_ARCH_RISCV64
  // TODO(RISCV): We need more information from the kernel to correctly mask
  // this address for RISC-V. https://github.com/v8-riscv/v8/issues/375
  raw_addr &= uint64_t{0xFFFFFF0000};
#elif V8_TARGET_ARCH_LOONG64
  // 42 bits of virtual addressing. Truncate to 40 bits to allow kernel chance
  // to fulfill request.
  raw_addr &= uint64_t{0xFFFFFF0000};
#else
  raw_addr &= 0x3FFFF000;

#ifdef __sun
  // For our Solaris/illumos mmap hint, we pick a random address in the bottom
  // half of the top half of the address space (that is, the third quarter).
  // Because we do not MAP_FIXED, this will be treated only as a hint -- the
  // system will not fail to mmap() because something else happens to already
  // be mapped at our random address. We deliberately set the hint high enough
  // to get well above the system's break (that is, the heap); Solaris and
  // illumos will try the hint and if that fails allocate as if there were
  // no hint at all. The high hint prevents the break from getting hemmed in
  // at low values, ceding half of the address space to the system heap.
  raw_addr += 0x80000000;
#elif V8_OS_AIX
  // The range 0x30000000 - 0xD0000000 is available on AIX;
  // choose the upper range.
  raw_addr += 0x90000000;
#else
  // The range 0x20000000 - 0x60000000 is relatively unpopulated across a
  // variety of ASLR modes (PAE kernel, NX compat mode, etc) and on macos
  // 10.6 and 10.7.
  raw_addr += 0x20000000;
#endif
#endif
#endif
  return reinterpret_cast<void*>(raw_addr);
}

// TODO(bbudge) Move Cygwin and Fuchsia stuff into platform-specific files.
#if !V8_OS_CYGWIN && !V8_OS_FUCHSIA
// static
void* OS::Allocate(void* hint, size_t size, size_t alignment,
                   MemoryPermission access) {
  size_t page_size = AllocatePageSize();
  DCHECK_EQ(0, size % page_size);
  DCHECK_EQ(0, alignment % page_size);
  hint = AlignedAddress(hint, alignment);
  // Add the maximum misalignment so we are guaranteed an aligned base address.
  size_t request_size = size + (alignment - page_size);
  request_size = RoundUp(request_size, OS::AllocatePageSize());
  void* result = base::Allocate(hint, request_size, access, PageType::kPrivate);
  if (result == nullptr) return nullptr;

  // Unmap memory allocated before the aligned base address.
  uint8_t* base = static_cast<uint8_t*>(result);
  uint8_t* aligned_base = reinterpret_cast<uint8_t*>(
      RoundUp(reinterpret_cast<uintptr_t>(base), alignment));
  if (aligned_base != base) {
    DCHECK_LT(base, aligned_base);
    size_t prefix_size = static_cast<size_t>(aligned_base - base);
    CHECK(Free(base, prefix_size));
    request_size -= prefix_size;
  }
  // Unmap memory allocated after the potentially unaligned end.
  if (size != request_size) {
    DCHECK_LT(size, request_size);
    size_t suffix_size = request_size - size;
    CHECK(Free(aligned_base + size, suffix_size));
    request_size -= suffix_size;
  }

  DCHECK_EQ(size, request_size);
  return static_cast<void*>(aligned_base);
}

// static
void* OS::AllocateShared(size_t size, MemoryPermission access) {
  DCHECK_EQ(0, size % AllocatePageSize());
  return base::Allocate(nullptr, size, access, PageType::kShared);
}

// static
bool OS::Free(void* address, const size_t size) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % AllocatePageSize());
  DCHECK_EQ(0, size % AllocatePageSize());
  return munmap(address, size) == 0;
}

// static
bool OS::Release(void* address, size_t size) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % CommitPageSize());
  DCHECK_EQ(0, size % CommitPageSize());
  return munmap(address, size) == 0;
}

// static
bool OS::SetPermissions(void* address, size_t size, MemoryPermission access) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % CommitPageSize());
  DCHECK_EQ(0, size % CommitPageSize());

  int prot = GetProtectionFromMemoryPermission(access);
  int ret = mprotect(address, size, prot);

  // MacOS 11.2 on Apple Silicon refuses to switch permissions from
  // rwx to none. Just use madvise instead.
#if defined(V8_OS_MACOSX)
  if (ret != 0 && access == OS::MemoryPermission::kNoAccess) {
    ret = madvise(address, size, MADV_FREE_REUSABLE);
    return ret == 0;
  }
#endif

  if (ret == 0 && access == OS::MemoryPermission::kNoAccess) {
    // This is advisory; ignore errors and continue execution.
    USE(DiscardSystemPages(address, size));
  }

// For accounting purposes, we want to call MADV_FREE_REUSE on macOS after
// changing permissions away from OS::MemoryPermission::kNoAccess. Since this
// state is not kept at this layer, we always call this if access != kNoAccess.
// The cost is a syscall that effectively no-ops.
// TODO(erikchen): Fix this to only call MADV_FREE_REUSE when necessary.
// https://crbug.com/823915
#if defined(V8_OS_MACOSX)
  if (access != OS::MemoryPermission::kNoAccess)
    madvise(address, size, MADV_FREE_REUSE);
#endif

  return ret == 0;
}

bool OS::DiscardSystemPages(void* address, size_t size) {
  DCHECK_EQ(0, reinterpret_cast<uintptr_t>(address) % CommitPageSize());
  DCHECK_EQ(0, size % CommitPageSize());
#if defined(V8_OS_MACOSX)
  // On OSX, MADV_FREE_REUSABLE has comparable behavior to MADV_FREE, but also
  // marks the pages with the reusable bit, which allows both Activity Monitor
  // and memory-infra to correctly track the pages.
  int ret = madvise(address, size, MADV_FREE_REUSABLE);
#elif defined(_AIX) || defined(V8_OS_SOLARIS)
  int ret = madvise(reinterpret_cast<caddr_t>(address), size, MADV_FREE);
#else
  int ret = madvise(address, size, MADV_FREE);
#endif
  if (ret != 0 && errno == ENOSYS)
    return true;  // madvise is not available on all systems.
  if (ret != 0 && errno == EINVAL) {
// MADV_FREE only works on Linux 4.5+ . If request failed, retry with older
// MADV_DONTNEED . Note that MADV_FREE being defined at compile time doesn't
// imply runtime support.
#if defined(_AIX) || defined(V8_OS_SOLARIS)
    ret = madvise(reinterpret_cast<caddr_t>(address), size, MADV_DONTNEED);
#else
    ret = madvise(address, size, MADV_DONTNEED);
#endif
  }
  return ret == 0;
}

// static
bool OS::HasLazyCommits() {
#if V8_OS_AIX || V8_OS_LINUX || V8_OS_MACOSX
  return true;
#else
  // TODO(bbudge) Return true for all POSIX platforms.
  return false;
#endif
}
#endif  // !V8_OS_CYGWIN && !V8_OS_FUCHSIA

const char* OS::GetGCFakeMMapFile() {
  return g_gc_fake_mmap;
}


void OS::Sleep(TimeDelta interval) {
  usleep(static_cast<useconds_t>(interval.InMicroseconds()));
}


void OS::Abort() {
  if (g_hard_abort) {
    IMMEDIATE_CRASH();
  }
  // Redirect to std abort to signal abnormal program termination.
  abort();
}


void OS::DebugBreak() {
#if V8_HOST_ARCH_ARM
  asm("bkpt 0");
#elif V8_HOST_ARCH_ARM64
  asm("brk 0");
#elif V8_HOST_ARCH_MIPS
  asm("break");
#elif V8_HOST_ARCH_MIPS64
  asm("break");
#elif V8_HOST_ARCH_LOONG64
  asm("break 0");
#elif V8_HOST_ARCH_PPC || V8_HOST_ARCH_PPC64
  asm("twge 2,2");
#elif V8_HOST_ARCH_IA32
  asm("int $3");
#elif V8_HOST_ARCH_X64
  asm("int $3");
#elif V8_HOST_ARCH_S390
  // Software breakpoint instruction is 0x0001
  asm volatile(".word 0x0001");
#elif V8_HOST_ARCH_RISCV64
  asm("ebreak");
#else
#error Unsupported host architecture.
#endif
}


class PosixMemoryMappedFile final : public OS::MemoryMappedFile {
 public:
  PosixMemoryMappedFile(FILE* file, void* memory, size_t size)
      : file_(file), memory_(memory), size_(size) {}
  ~PosixMemoryMappedFile() final;
  void* memory() const final { return memory_; }
  size_t size() const final { return size_; }

 private:
  FILE* const file_;
  void* const memory_;
  size_t const size_;
};


// static
OS::MemoryMappedFile* OS::MemoryMappedFile::open(const char* name,
                                                 FileMode mode) {
  const char* fopen_mode = (mode == FileMode::kReadOnly) ? "r" : "r+";
  if (FILE* file = fopen(name, fopen_mode)) {
    if (fseek(file, 0, SEEK_END) == 0) {
      long size = ftell(file);  // NOLINT(runtime/int)
      if (size == 0) return new PosixMemoryMappedFile(file, nullptr, 0);
      if (size > 0) {
        int prot = PROT_READ;
        int flags = MAP_PRIVATE;
        if (mode == FileMode::kReadWrite) {
          prot |= PROT_WRITE;
          flags = MAP_SHARED;
        }
        void* const memory =
            mmap(OS::GetRandomMmapAddr(), size, prot, flags, fileno(file), 0);
        if (memory != MAP_FAILED) {
          return new PosixMemoryMappedFile(file, memory, size);
        }
      }
    }
    fclose(file);
  }
  return nullptr;
}

// static
OS::MemoryMappedFile* OS::MemoryMappedFile::create(const char* name,
                                                   size_t size, void* initial) {
  if (FILE* file = fopen(name, "w+")) {
    if (size == 0) return new PosixMemoryMappedFile(file, nullptr, 0);
    size_t result = fwrite(initial, 1, size, file);
    if (result == size && !ferror(file)) {
      void* memory = mmap(OS::GetRandomMmapAddr(), result,
                          PROT_READ | PROT_WRITE, MAP_SHARED, fileno(file), 0);
      if (memory != MAP_FAILED) {
        return new PosixMemoryMappedFile(file, memory, result);
      }
    }
    fclose(file);
  }
  return nullptr;
}


PosixMemoryMappedFile::~PosixMemoryMappedFile() {
  if (memory_) CHECK(OS::Free(memory_, RoundUp(size_, OS::AllocatePageSize())));
  fclose(file_);
}


int OS::GetCurrentProcessId() {
  return static_cast<int>(getpid());
}


int OS::GetCurrentThreadId() {
#if V8_OS_MACOSX || (V8_OS_ANDROID && defined(__APPLE__))
  return static_cast<int>(pthread_mach_thread_np(pthread_self()));
#elif V8_OS_LINUX
  return static_cast<int>(syscall(__NR_gettid));
#elif V8_OS_ANDROID
  return static_cast<int>(gettid());
#elif V8_OS_AIX
  return static_cast<int>(thread_self());
#elif V8_OS_FUCHSIA
  return static_cast<int>(zx_thread_self());
#elif V8_OS_SOLARIS
  return static_cast<int>(pthread_self());
#else
  return static_cast<int>(reinterpret_cast<intptr_t>(pthread_self()));
#endif
}

void OS::ExitProcess(int exit_code) {
  // Use _exit instead of exit to avoid races between isolate
  // threads and static destructors.
  fflush(stdout);
  fflush(stderr);
  _exit(exit_code);
}

// ----------------------------------------------------------------------------
// POSIX date/time support.
//

#if !defined(V8_OS_FUCHSIA)
int OS::GetUserTime(uint32_t* secs, uint32_t* usecs) {
  struct rusage usage;

  if (getrusage(RUSAGE_SELF, &usage) < 0) return -1;
  *secs = static_cast<uint32_t>(usage.ru_utime.tv_sec);
  *usecs = static_cast<uint32_t>(usage.ru_utime.tv_usec);
  return 0;
}
#endif

double OS::TimeCurrentMillis() {
  return Time::Now().ToJsTime();
}

double PosixTimezoneCache::DaylightSavingsOffset(double time) {
  if (std::isnan(time)) return std::numeric_limits<double>::quiet_NaN();
  time_t tv = static_cast<time_t>(std::floor(time/msPerSecond));
  struct tm tm;
  struct tm* t = localtime_r(&tv, &tm);
  if (nullptr == t) return std::numeric_limits<double>::quiet_NaN();
  return t->tm_isdst > 0 ? 3600 * msPerSecond : 0;
}


int OS::GetLastError() {
  return errno;
}


// ----------------------------------------------------------------------------
// POSIX stdio support.
//

FILE* OS::FOpen(const char* path, const char* mode) {
  FILE* file = fopen(path, mode);
  if (file == nullptr) return nullptr;
  struct stat file_stat;
  if (fstat(fileno(file), &file_stat) != 0) {
    fclose(file);
    return nullptr;
  }
  bool is_regular_file = ((file_stat.st_mode & S_IFREG) != 0);
  if (is_regular_file) return file;
  fclose(file);
  return nullptr;
}


bool OS::Remove(const char* path) {
  return (remove(path) == 0);
}

char OS::DirectorySeparator() { return '/'; }

bool OS::isDirectorySeparator(const char ch) {
  return ch == DirectorySeparator();
}


FILE* OS::OpenTemporaryFile() {
  return tmpfile();
}

const char* const OS::LogFileOpenMode = "w+";

void OS::Print(const char* format, ...) {
  va_list args;
  va_start(args, format);
  VPrint(format, args);
  va_end(args);
}


void OS::VPrint(const char* format, va_list args) {
#if defined(ANDROID) && !defined(V8_ANDROID_LOG_STDOUT)
  __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, format, args);
#else
  vprintf(format, args);
#endif
}


void OS::FPrint(FILE* out, const char* format, ...) {
  va_list args;
  va_start(args, format);
  VFPrint(out, format, args);
  va_end(args);
}


void OS::VFPrint(FILE* out, const char* format, va_list args) {
#if defined(ANDROID) && !defined(V8_ANDROID_LOG_STDOUT)
  __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, format, args);
#else
  vfprintf(out, format, args);
#endif
}


void OS::PrintError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  VPrintError(format, args);
  va_end(args);
}


void OS::VPrintError(const char* format, va_list args) {
#if defined(ANDROID) && !defined(V8_ANDROID_LOG_STDOUT)
  __android_log_vprint(ANDROID_LOG_ERROR, LOG_TAG, format, args);
#else
  vfprintf(stderr, format, args);
#endif
}


int OS::SNPrintF(char* str, int length, const char* format, ...) {
  va_list args;
  va_start(args, format);
  int result = VSNPrintF(str, length, format, args);
  va_end(args);
  return result;
}


int OS::VSNPrintF(char* str,
                  int length,
                  const char* format,
                  va_list args) {
  int n = vsnprintf(str, length, format, args);
  if (n < 0 || n >= length) {
    // If the length is zero, the assignment fails.
    if (length > 0)
      str[length - 1] = '\0';
    return -1;
  } else {
    return n;
  }
}


// ----------------------------------------------------------------------------
// POSIX string support.
//

void OS::StrNCpy(char* dest, int length, const char* src, size_t n) {
  strncpy(dest, src, n);
}


// ----------------------------------------------------------------------------
// POSIX thread support.
//

class Thread::PlatformData {
 public:
  PlatformData() : thread_(kNoThread) {}
  pthread_t thread_;  // Thread handle for pthread.
  // Synchronizes thread creation
  Mutex thread_creation_mutex_;
};

Thread::Thread(const Options& options)
    : data_(new PlatformData),
      stack_size_(options.stack_size()),
      start_semaphore_(nullptr) {
  if (stack_size_ > 0 && static_cast<size_t>(stack_size_) < PTHREAD_STACK_MIN) {
    stack_size_ = PTHREAD_STACK_MIN;
  }
  set_name(options.name());
}


Thread::~Thread() {
  delete data_;
}


static void SetThreadName(const char* name) {
#if V8_OS_DRAGONFLYBSD || V8_OS_FREEBSD || V8_OS_OPENBSD
  pthread_set_name_np(pthread_self(), name);
#elif V8_OS_NETBSD
  STATIC_ASSERT(Thread::kMaxThreadNameLength <= PTHREAD_MAX_NAMELEN_NP);
  pthread_setname_np(pthread_self(), "%s", name);
#elif V8_OS_MACOSX
  // pthread_setname_np is only available in 10.6 or later, so test
  // for it at runtime.
  int (*dynamic_pthread_setname_np)(const char*);
  *reinterpret_cast<void**>(&dynamic_pthread_setname_np) =
    dlsym(RTLD_DEFAULT, "pthread_setname_np");
  if (dynamic_pthread_setname_np == nullptr) return;

  // Mac OS X does not expose the length limit of the name, so hardcode it.
  static const int kMaxNameLength = 63;
  STATIC_ASSERT(Thread::kMaxThreadNameLength <= kMaxNameLength);
  dynamic_pthread_setname_np(name);
#elif defined(PR_SET_NAME)
  prctl(PR_SET_NAME,
        reinterpret_cast<unsigned long>(name),  // NOLINT
        0, 0, 0);
#endif
}


static void* ThreadEntry(void* arg) {
  Thread* thread = reinterpret_cast<Thread*>(arg);
  // We take the lock here to make sure that pthread_create finished first since
  // we don't know which thread will run first (the original thread or the new
  // one).
  { MutexGuard lock_guard(&thread->data()->thread_creation_mutex_); }
  SetThreadName(thread->name());
  DCHECK_NE(thread->data()->thread_, kNoThread);
  thread->NotifyStartedAndRun();
  return nullptr;
}


void Thread::set_name(const char* name) {
  strncpy(name_, name, sizeof(name_) - 1);
  name_[sizeof(name_) - 1] = '\0';
}

bool Thread::Start() {
  int result;
  pthread_attr_t attr;
  memset(&attr, 0, sizeof(attr));
  result = pthread_attr_init(&attr);
  if (result != 0) return false;
  size_t stack_size = stack_size_;
  if (stack_size == 0) {
#if V8_OS_MACOSX
    // Default on Mac OS X is 512kB -- bump up to 1MB
    stack_size = 1 * 1024 * 1024;
#elif V8_OS_AIX
    // Default on AIX is 96kB -- bump up to 2MB
    stack_size = 2 * 1024 * 1024;
#endif
  }
  if (stack_size > 0) {
    result = pthread_attr_setstacksize(&attr, stack_size);
    if (result != 0) return pthread_attr_destroy(&attr), false;
  }
  {
    MutexGuard lock_guard(&data_->thread_creation_mutex_);
    result = pthread_create(&data_->thread_, &attr, ThreadEntry, this);
    if (result != 0 || data_->thread_ == kNoThread) {
      return pthread_attr_destroy(&attr), false;
    }
  }
  result = pthread_attr_destroy(&attr);
  return result == 0;
}

void Thread::Join() { pthread_join(data_->thread_, nullptr); }

static Thread::LocalStorageKey PthreadKeyToLocalKey(pthread_key_t pthread_key) {
#if V8_OS_CYGWIN
  // We need to cast pthread_key_t to Thread::LocalStorageKey in two steps
  // because pthread_key_t is a pointer type on Cygwin. This will probably not
  // work on 64-bit platforms, but Cygwin doesn't support 64-bit anyway.
  STATIC_ASSERT(sizeof(Thread::LocalStorageKey) == sizeof(pthread_key_t));
  intptr_t ptr_key = reinterpret_cast<intptr_t>(pthread_key);
  return static_cast<Thread::LocalStorageKey>(ptr_key);
#else
  return static_cast<Thread::LocalStorageKey>(pthread_key);
#endif
}


static pthread_key_t LocalKeyToPthreadKey(Thread::LocalStorageKey local_key) {
#if V8_OS_CYGWIN
  STATIC_ASSERT(sizeof(Thread::LocalStorageKey) == sizeof(pthread_key_t));
  intptr_t ptr_key = static_cast<intptr_t>(local_key);
  return reinterpret_cast<pthread_key_t>(ptr_key);
#else
  return static_cast<pthread_key_t>(local_key);
#endif
}


#ifdef V8_FAST_TLS_SUPPORTED

static std::atomic<bool> tls_base_offset_initialized{false};
intptr_t kMacTlsBaseOffset = 0;

// It's safe to do the initialization more that once, but it has to be
// done at least once.
static void InitializeTlsBaseOffset() {
  const size_t kBufferSize = 128;
  char buffer[kBufferSize];
  size_t buffer_size = kBufferSize;
  int ctl_name[] = { CTL_KERN , KERN_OSRELEASE };
  if (sysctl(ctl_name, 2, buffer, &buffer_size, nullptr, 0) != 0) {
    FATAL("V8 failed to get kernel version");
  }
  // The buffer now contains a string of the form XX.YY.ZZ, where
  // XX is the major kernel version component.
  // Make sure the buffer is 0-terminated.
  buffer[kBufferSize - 1] = '\0';
  char* period_pos = strchr(buffer, '.');
  *period_pos = '\0';
  int kernel_version_major = static_cast<int>(strtol(buffer, nullptr, 10));
  // The constants below are taken from pthreads.s from the XNU kernel
  // sources archive at www.opensource.apple.com.
  if (kernel_version_major < 11) {
    // 8.x.x (Tiger), 9.x.x (Leopard), 10.x.x (Snow Leopard) have the
    // same offsets.
#if V8_HOST_ARCH_IA32
    kMacTlsBaseOffset = 0x48;
#else
    kMacTlsBaseOffset = 0x60;
#endif
  } else {
    // 11.x.x (Lion) changed the offset.
    kMacTlsBaseOffset = 0;
  }

  tls_base_offset_initialized.store(true, std::memory_order_release);
}


static void CheckFastTls(Thread::LocalStorageKey key) {
  void* expected = reinterpret_cast<void*>(0x1234CAFE);
  Thread::SetThreadLocal(key, expected);
  void* actual = Thread::GetExistingThreadLocal(key);
  if (expected != actual) {
    FATAL("V8 failed to initialize fast TLS on current kernel");
  }
  Thread::SetThreadLocal(key, nullptr);
}

#endif  // V8_FAST_TLS_SUPPORTED


Thread::LocalStorageKey Thread::CreateThreadLocalKey() {
#ifdef V8_FAST_TLS_SUPPORTED
  bool check_fast_tls = false;
  if (!tls_base_offset_initialized.load(std::memory_order_acquire)) {
    check_fast_tls = true;
    InitializeTlsBaseOffset();
  }
#endif
  pthread_key_t key;
  int result = pthread_key_create(&key, nullptr);
  DCHECK_EQ(0, result);
  USE(result);
  LocalStorageKey local_key = PthreadKeyToLocalKey(key);
#ifdef V8_FAST_TLS_SUPPORTED
  // If we just initialized fast TLS support, make sure it works.
  if (check_fast_tls) CheckFastTls(local_key);
#endif
  return local_key;
}


void Thread::DeleteThreadLocalKey(LocalStorageKey key) {
  pthread_key_t pthread_key = LocalKeyToPthreadKey(key);
  int result = pthread_key_delete(pthread_key);
  DCHECK_EQ(0, result);
  USE(result);
}


void* Thread::GetThreadLocal(LocalStorageKey key) {
  pthread_key_t pthread_key = LocalKeyToPthreadKey(key);
  return pthread_getspecific(pthread_key);
}


void Thread::SetThreadLocal(LocalStorageKey key, void* value) {
  pthread_key_t pthread_key = LocalKeyToPthreadKey(key);
  int result = pthread_setspecific(pthread_key, value);
  DCHECK_EQ(0, result);
  USE(result);
}

// pthread_getattr_np used below is non portable (hence the _np suffix). We
// keep this version in POSIX as most Linux-compatible derivatives will
// support it. MacOS and FreeBSD are different here.
#if !defined(V8_OS_FREEBSD) && !defined(V8_OS_MACOSX) && !defined(_AIX) && \
    !defined(V8_OS_SOLARIS)

// static
Stack::StackSlot Stack::GetStackStart() {
  pthread_attr_t attr;
  int error = pthread_getattr_np(pthread_self(), &attr);
  if (!error) {
    void* base;
    size_t size;
    error = pthread_attr_getstack(&attr, &base, &size);
    CHECK(!error);
    pthread_attr_destroy(&attr);
    return reinterpret_cast<uint8_t*>(base) + size;
  }

#if defined(V8_LIBC_GLIBC)
  // pthread_getattr_np can fail for the main thread. In this case
  // just like NaCl we rely on the __libc_stack_end to give us
  // the start of the stack.
  // See https://code.google.com/p/nativeclient/issues/detail?id=3431.
  return __libc_stack_end;
#else
  return nullptr;
#endif  // !defined(V8_LIBC_GLIBC)
}

#endif  // !defined(V8_OS_FREEBSD) && !defined(V8_OS_MACOSX) &&
        // !defined(_AIX) && !defined(V8_OS_SOLARIS)

// static
Stack::StackSlot Stack::GetCurrentStackPosition() {
  return __builtin_frame_address(0);
}

#undef LOG_TAG
#undef MAP_ANONYMOUS
#undef MADV_FREE

}  // namespace base
}  // namespace v8
