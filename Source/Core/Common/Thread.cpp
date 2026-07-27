// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Thread.h"

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined BSD4_4 || defined __FreeBSD__ || defined __OpenBSD__
#include <pthread_np.h>
#elif defined __NetBSD__
#include <sched.h>
#elif defined __HAIKU__
#include <OS.h>
#endif

#ifdef USE_VTUNE
#include <ittnotify.h>
#pragma comment(lib, "libittnotify.lib")
#endif

#include "Common/CommonTypes.h"
#ifdef _WIN32
#include "Common/StringUtil.h"
#endif

namespace Common
{
int CurrentThreadId()
{
#ifdef _WIN32
  return GetCurrentThreadId();
#elif defined __APPLE__
  return mach_thread_self();
#else
  return 0;
#endif
}

#ifdef _WIN32

void SetThreadAffinity(std::thread::native_handle_type thread, u32 mask)
{
  SetThreadAffinityMask(thread, mask);
}

void SetCurrentThreadAffinity(u32 mask)
{
  SetThreadAffinityMask(GetCurrentThread(), mask);
}

// Supporting functions
void SleepCurrentThread(int ms)
{
  Sleep(ms);
}

void SwitchCurrentThread()
{
  SwitchToThread();
}

// Sets the debugger-visible name of the current thread.
// Uses trick documented in:
// https://docs.microsoft.com/en-us/visualstudio/debugger/how-to-set-a-thread-name-in-native-code
static void SetCurrentThreadNameViaException(const char* name)
{
  static const DWORD MS_VC_EXCEPTION = 0x406D1388;

#pragma pack(push, 8)
  struct THREADNAME_INFO
  {
    DWORD dwType;      // must be 0x1000
    LPCSTR szName;     // pointer to name (in user addr space)
    DWORD dwThreadID;  // thread ID (-1=caller thread)
    DWORD dwFlags;     // reserved for future use, must be zero
  } info;
#pragma pack(pop)

  info.dwType = 0x1000;
  info.szName = name;
  info.dwThreadID = static_cast<DWORD>(-1);
  info.dwFlags = 0;

  __try
  {
    RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
  }
  __except (EXCEPTION_CONTINUE_EXECUTION)
  {
  }
}

static void SetCurrentThreadNameViaApi(const char* name)
{
  // If possible, also set via the newer API. On some versions of Server it needs to be manually
  // resolved. This API allows being able to observe the thread name even if a debugger wasn't
  // attached when the name was set (see above link for more info).
  static auto pSetThreadDescription = (decltype(&SetThreadDescription))GetProcAddress(
      GetModuleHandleA("kernel32"), "SetThreadDescription");
  if (pSetThreadDescription)
  {
    pSetThreadDescription(GetCurrentThread(), UTF8ToWString(name).c_str());
  }
}

void SetCurrentThreadName(const char* name)
{
  SetCurrentThreadNameViaException(name);
  SetCurrentThreadNameViaApi(name);
}

#else  // !WIN32, so must be POSIX threads

void SetThreadAffinity(std::thread::native_handle_type thread, u32 mask)
{
#ifdef __SWITCH__
  (void)thread;
#elif defined(__APPLE__)
  thread_policy_set(pthread_mach_thread_np(thread), THREAD_AFFINITY_POLICY, (integer_t*)&mask, 1);
#elif (defined __linux__ || defined BSD4_4 || defined __FreeBSD__ || defined __NetBSD__) &&        \
    !(defined ANDROID)
#ifndef __NetBSD__
#ifdef __FreeBSD__
  cpuset_t cpu_set;
#else
  cpu_set_t cpu_set;
#endif
  CPU_ZERO(&cpu_set);

  for (int i = 0; i != sizeof(mask) * 8; ++i)
    if ((mask >> i) & 1)
      CPU_SET(i, &cpu_set);

  pthread_setaffinity_np(thread, sizeof(cpu_set), &cpu_set);
#else
  cpuset_t* cpu_set = cpuset_create();

  for (int i = 0; i != sizeof(mask) * 8; ++i)
    if ((mask >> i) & 1)
      cpuset_set(i, cpu_set);

  pthread_setaffinity_np(thread, cpuset_size(cpu_set), cpu_set);
  cpuset_destroy(cpu_set);
#endif
#endif
}

void SetCurrentThreadAffinity(u32 mask)
{
#ifdef __SWITCH__
  int32_t core = static_cast<int32_t>(mask);
  if (core > 2)
    core = 2;
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, (1ULL << core));
#else
  SetThreadAffinity(pthread_self(), mask);
#endif
}

bool AdjustCurrentThreadPriority(int offset)
{
#ifdef __SWITCH__
  s32 current_priority = 0;
  if (R_FAILED(svcGetThreadPriority(&current_priority, CUR_THREAD_HANDLE)))
    return false;

  // Horizon uses 6-bit priorities.
  const s64 adjusted = static_cast<s64>(current_priority) + offset;
  const u32 priority = static_cast<u32>(adjusted < 0 ? 0 : adjusted > 0x3f ? 0x3f : adjusted);
  return priority == static_cast<u32>(current_priority) ||
         R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, priority));
#else
  (void)offset;
  return false;
#endif
}

void SleepCurrentThread(int ms)
{
  usleep(1000 * ms);
}

void SwitchCurrentThread()
{
  usleep(1000 * 1);
}

void SetCurrentThreadName(const char* name)
{
#ifdef __APPLE__
  pthread_setname_np(name);
#elif defined __FreeBSD__ || defined __OpenBSD__
  pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
  pthread_setname_np(pthread_self(), "%s", const_cast<char*>(name));
#elif defined __HAIKU__
  rename_thread(find_thread(nullptr), name);
#elif defined(__SWITCH__)
  // Unpinned workers share application core 2.
  SetCurrentThreadAffinity(2);
  (void)name;
#else
  // linux doesn't allow to set more than 16 bytes, including \0.
  pthread_setname_np(pthread_self(), std::string(name).substr(0, 15).c_str());
#endif
#ifdef USE_VTUNE
  // VTune uses OS thread names by default but probably supports longer names when set via its own
  // API.
  __itt_thread_set_name(name);
#endif
}

std::tuple<void*, size_t> GetCurrentThreadStack()
{
  void* stack_addr;
  size_t stack_size;

#ifdef __SWITCH__
  void* sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));

  Thread* self = threadGetSelf();

  if (self && self->stack_sz != 0)
  {
    const uintptr_t sp_addr = reinterpret_cast<uintptr_t>(sp);
    const auto contains_sp = [sp_addr, self](void* base) {
      if (!base)
        return false;
      const uintptr_t start = reinterpret_cast<uintptr_t>(base);
      return sp_addr >= start && sp_addr < start + self->stack_sz;
    };

    stack_size = self->stack_sz;
    if (contains_sp(self->stack_mem))
      stack_addr = self->stack_mem;
    else if (contains_sp(self->stack_mirror))
      stack_addr = self->stack_mirror;
    else if (self->stack_mem)
      stack_addr = self->stack_mem;
    else
      stack_addr = self->stack_mirror;
  }
  else
  {
    stack_size = 0x200000;
    stack_addr = reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(sp) & ~0xFFFULL) - stack_size + 0x1000);
  }
#else
  pthread_t self = pthread_self();

#ifdef __APPLE__
  stack_size = pthread_get_stacksize_np(self);
  stack_addr = reinterpret_cast<u8*>(pthread_get_stackaddr_np(self)) - stack_size;
#elif defined __OpenBSD__
  stack_t stack;
  pthread_stackseg_np(self, &stack);

  stack_addr = reinterpret_cast<u8*>(stack.ss_sp) - stack.ss_size;
  stack_size = stack.ss_size;
#else
  pthread_attr_t attr;

#ifdef __FreeBSD__
  pthread_attr_init(&attr);
  pthread_attr_get_np(self, &attr);
#else
  // Linux and NetBSD
  pthread_getattr_np(self, &attr);
#endif

  pthread_attr_getstack(&attr, &stack_addr, &stack_size);

  pthread_attr_destroy(&attr);
#endif
#endif  // __SWITCH__

  return std::make_tuple(stack_addr, stack_size);
}

#endif

}  // namespace Common
