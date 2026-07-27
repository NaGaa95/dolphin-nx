// Copyright 2008 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemTools.h"

#include <cstdlib>

#include "Common/CommonFuncs.h"
#include "Common/MsgHandler.h"

#include "Core/MachineContext.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/System.h"

#ifdef __SWITCH__
#include <atomic>

#include <switch.h>

static std::atomic<uintptr_t> s_lazy_region_base{0};
static std::atomic<size_t> s_lazy_region_size{0};

void EMM::SetLazyRegionInfo(uintptr_t base, size_t size)
{
  if (base == 0)
  {
    s_lazy_region_base.store(0, std::memory_order_release);
    s_lazy_region_size.store(0, std::memory_order_relaxed);
  }
  else
  {
    s_lazy_region_size.store(size, std::memory_order_relaxed);
    s_lazy_region_base.store(base, std::memory_order_release);
  }
}
#endif

#if defined(__FreeBSD__) || defined(__NetBSD__)
#include <signal.h>
#endif
#ifndef _WIN32
#include <unistd.h>  // Needed for _POSIX_VERSION
#endif

#ifdef _WIN32
#include "Common/Assert.h"
#endif
#if defined(__APPLE__) && !defined(USE_SIGACTION_ON_APPLE)
#include "Common/Thread.h"
#endif

#if defined(__APPLE__)
#ifdef _M_X86_64
#define THREAD_STATE64_COUNT x86_THREAD_STATE64_COUNT
#define THREAD_STATE64 x86_THREAD_STATE64
#define thread_state64_t x86_thread_state64_t
#elif defined(_M_ARM_64)
#define THREAD_STATE64_COUNT ARM_THREAD_STATE64_COUNT
#define THREAD_STATE64 ARM_THREAD_STATE64
#define thread_state64_t arm_thread_state64_t
#else
#error Unsupported architecture
#endif
#endif

namespace EMM
{
#ifdef _WIN32

static PVOID s_veh_handle;

static LONG NTAPI Handler(PEXCEPTION_POINTERS pPtrs)
{
  switch (pPtrs->ExceptionRecord->ExceptionCode)
  {
  case EXCEPTION_ACCESS_VIOLATION:
  {
    ULONG_PTR access_type = pPtrs->ExceptionRecord->ExceptionInformation[0];
    if (access_type == 8)  // Rule out DEP
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    // virtual address of the inaccessible data
    uintptr_t fault_address = (uintptr_t)pPtrs->ExceptionRecord->ExceptionInformation[1];
    SContext* ctx = pPtrs->ContextRecord;

    if (Core::System::GetInstance().GetJitInterface().HandleFault(fault_address, ctx))
    {
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    else
    {
      // Let's not prevent debugging.
      return EXCEPTION_CONTINUE_SEARCH;
    }
  }

  case EXCEPTION_STACK_OVERFLOW:
    if (Core::System::GetInstance().GetJitInterface().HandleStackFault())
      return EXCEPTION_CONTINUE_EXECUTION;
    else
      return EXCEPTION_CONTINUE_SEARCH;

  case EXCEPTION_ILLEGAL_INSTRUCTION:
    // No SSE support? Or simply bad codegen?
    return EXCEPTION_CONTINUE_SEARCH;

  case EXCEPTION_PRIV_INSTRUCTION:
    // okay, dynarec codegen is obviously broken.
    return EXCEPTION_CONTINUE_SEARCH;

  case EXCEPTION_IN_PAGE_ERROR:
    // okay, something went seriously wrong, out of memory?
    return EXCEPTION_CONTINUE_SEARCH;

  case EXCEPTION_BREAKPOINT:
    // might want to do something fun with this one day?
    return EXCEPTION_CONTINUE_SEARCH;

  default:
    return EXCEPTION_CONTINUE_SEARCH;
  }
}

void InstallExceptionHandler()
{
  ASSERT(!s_veh_handle);
  s_veh_handle = AddVectoredExceptionHandler(TRUE, Handler);
  ASSERT(s_veh_handle);
}

void UninstallExceptionHandler()
{
  ULONG status = RemoveVectoredExceptionHandler(s_veh_handle);
  ASSERT(status);
  if (status)
    s_veh_handle = nullptr;
}

bool IsExceptionHandlerSupported()
{
  return true;
}

#elif defined(__APPLE__) && !defined(USE_SIGACTION_ON_APPLE)

static void CheckKR(const char* name, kern_return_t kr)
{
  if (kr)
  {
    PanicAlertFmt("{} failed: kr={:x}", name, kr);
  }
}

static void ExceptionThread(mach_port_t port)
{
  Common::SetCurrentThreadName("Mach exception thread");
#pragma pack(4)
  struct
  {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    exception_type_t exception;
    mach_msg_type_number_t codeCnt;
    int64_t code[2];
    int flavor;
    mach_msg_type_number_t old_stateCnt;
    natural_t old_state[THREAD_STATE64_COUNT];
    mach_msg_trailer_t trailer;
  } msg_in;

  struct
  {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
    int flavor;
    mach_msg_type_number_t new_stateCnt;
    natural_t new_state[THREAD_STATE64_COUNT];
  } msg_out;
#pragma pack()
  memset(&msg_in, 0xee, sizeof(msg_in));
  memset(&msg_out, 0xee, sizeof(msg_out));
  mach_msg_size_t send_size = 0;
  mach_msg_option_t option = MACH_RCV_MSG;
  while (true)
  {
    // If this isn't the first run, send the reply message.  Then, receive
    // a message: either a mach_exception_raise_state RPC due to
    // thread_set_exception_ports, or MACH_NOTIFY_NO_SENDERS due to
    // mach_port_request_notification.
    CheckKR("mach_msg_overwrite",
            mach_msg_overwrite(&msg_out.Head, option, send_size, sizeof(msg_in), port,
                               MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL, &msg_in.Head, 0));

    if (msg_in.Head.msgh_id == MACH_NOTIFY_NO_SENDERS)
    {
      // the other thread exited
      mach_port_destroy(mach_task_self(), port);
      return;
    }

    if (msg_in.Head.msgh_id != 2406)
    {
      PanicAlertFmt("unknown message received");
      return;
    }

    if (msg_in.flavor != THREAD_STATE64)
    {
      PanicAlertFmt("unknown flavor {} (expected {})", msg_in.flavor, THREAD_STATE64);
      return;
    }

    thread_state64_t* state = (thread_state64_t*)msg_in.old_state;

    bool ok =
        Core::System::GetInstance().GetJitInterface().HandleFault((uintptr_t)msg_in.code[1], state);

    // Set up the reply.
    msg_out.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(msg_in.Head.msgh_bits), 0);
    msg_out.Head.msgh_remote_port = msg_in.Head.msgh_remote_port;
    msg_out.Head.msgh_local_port = MACH_PORT_NULL;
    msg_out.Head.msgh_id = msg_in.Head.msgh_id + 100;
    msg_out.NDR = msg_in.NDR;
    if (ok)
    {
      msg_out.RetCode = KERN_SUCCESS;
      msg_out.flavor = THREAD_STATE64;
      msg_out.new_stateCnt = THREAD_STATE64_COUNT;
      memcpy(msg_out.new_state, msg_in.old_state, THREAD_STATE64_COUNT * sizeof(natural_t));
    }
    else
    {
      // Pass the exception to the next handler (debugger or crash).
      msg_out.RetCode = KERN_FAILURE;
      msg_out.flavor = 0;
      msg_out.new_stateCnt = 0;
    }
    msg_out.Head.msgh_size =
        offsetof(__typeof__(msg_out), new_state) + msg_out.new_stateCnt * sizeof(natural_t);

    send_size = msg_out.Head.msgh_size;
    option |= MACH_SEND_MSG;
  }
}

void InstallExceptionHandler()
{
  mach_port_t port;
  CheckKR("mach_port_allocate",
          mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
  std::thread exc_thread(ExceptionThread, port);
  exc_thread.detach();
  // Obtain a send right for thread_set_exception_ports to copy...
  CheckKR("mach_port_insert_right",
          mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
  // Mach tries the following exception ports in order: thread, task, host.
  // Debuggers set the task port, so we grab the thread port.
  CheckKR("thread_set_exception_ports",
          thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_ACCESS, port,
                                     EXCEPTION_STATE | MACH_EXCEPTION_CODES, THREAD_STATE64));
  // ...and get rid of our copy so that MACH_NOTIFY_NO_SENDERS works.
  CheckKR("mach_port_mod_refs",
          mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, -1));
  mach_port_t previous;
  CheckKR("mach_port_request_notification",
          mach_port_request_notification(mach_task_self(), port, MACH_NOTIFY_NO_SENDERS, 0, port,
                                         MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous));
}

void UninstallExceptionHandler()
{
}

bool IsExceptionHandlerSupported()
{
  return true;
}

#elif defined(_POSIX_VERSION) && !defined(_M_GENERIC)

static struct sigaction old_sa_segv;
#if defined(__APPLE__)
static struct sigaction old_sa_bus;
#endif

static void sigsegv_handler(int sig, siginfo_t* info, void* raw_context)
{
  if (sig != SIGSEGV
#if defined(__APPLE__)
      && sig != SIGBUS
#endif
  )
  {
    // We are not interested in other signals - handle it as usual.
    return;
  }
  auto* const context = static_cast<ucontext_t*>(raw_context);
  const int sicode = info->si_code;
  if (sicode != SEGV_MAPERR && sicode != SEGV_ACCERR)
  {
    // Huh? Return.
    return;
  }
  const auto bad_address = reinterpret_cast<uintptr_t>(info->si_addr);

// Get all the information we can out of the context.
#ifdef __OpenBSD__
  SContext* const ctx = context;
#elif defined(__APPLE__)
  // `uc_mcontext` is already a pointer here.
  SContext* const ctx = context->uc_mcontext;
#else
  SContext* const ctx = &context->uc_mcontext;
#endif
  if (Core::System::GetInstance().GetJitInterface().HandleFault(bad_address, ctx))
    return;

  // If JIT didn't handle the signal, restore the original handler and invoke it.
  const auto& old_sa =
#if defined(__APPLE__)
      (sig == SIGBUS) ? old_sa_bus :
#endif
                        old_sa_segv;

  sigaction(sig, &old_sa, nullptr);
  raise(sig);
}

void InstallExceptionHandler()
{
  stack_t signal_stack;
#ifdef __FreeBSD__
  signal_stack.ss_sp = (char*)malloc(SIGSTKSZ);
#else
  signal_stack.ss_sp = malloc(SIGSTKSZ);
#endif
  signal_stack.ss_size = SIGSTKSZ;
  signal_stack.ss_flags = 0;
  if (sigaltstack(&signal_stack, nullptr) != 0)
    PanicAlertFmt("sigaltstack failed: {}", Common::LastStrerrorString());
  struct sigaction sa{};
  sa.sa_sigaction = &sigsegv_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &old_sa_segv);
#ifdef __APPLE__
  sigaction(SIGBUS, &sa, &old_sa_bus);
#endif
}

void UninstallExceptionHandler()
{
  stack_t signal_stack;
  stack_t old_stack;
  signal_stack.ss_flags = SS_DISABLE;
  if (!sigaltstack(&signal_stack, &old_stack) && !(old_stack.ss_flags & SS_DISABLE))
  {
    free(old_stack.ss_sp);
  }
  sigaction(SIGSEGV, &old_sa_segv, nullptr);
#ifdef __APPLE__
  sigaction(SIGBUS, &old_sa_bus, nullptr);
#endif
}

bool IsExceptionHandlerSupported()
{
  return true;
}

#elif defined(__SWITCH__)

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx);

[[noreturn]] static void RestoreContextAndJump(ThreadExceptionDump* ctx)
{
  __asm__ volatile(
      "mov x21, %0\n"
      "ldp q0,  q1,  [x21, #288]\n"
      "ldp q2,  q3,  [x21, #320]\n"
      "ldp q4,  q5,  [x21, #352]\n"
      "ldp q6,  q7,  [x21, #384]\n"
      "ldp q8,  q9,  [x21, #416]\n"
      "ldp q10, q11, [x21, #448]\n"
      "ldp q12, q13, [x21, #480]\n"
      "ldp q14, q15, [x21, #512]\n"
      "ldp q16, q17, [x21, #544]\n"
      "ldp q18, q19, [x21, #576]\n"
      "ldp q20, q21, [x21, #608]\n"
      "ldp q22, q23, [x21, #640]\n"
      "ldp q24, q25, [x21, #672]\n"
      "ldp q26, q27, [x21, #704]\n"
      "ldp q28, q29, [x21, #736]\n"
      "ldp q30, q31, [x21, #768]\n"
      "ldr w16, [x21, #800]\n"
      "msr nzcv, x16\n"
      "ldr x16, [x21, #264]\n"
      "ldr x18, [x21, #272]\n"
      "str x18, [x16, #-16]!\n"
      "mov x18, x16\n"
      "ldr x30, [x21, #256]\n"
      "ldr x29, [x21, #248]\n"
      "ldp x0,  x1,  [x21, #16]\n"
      "ldp x2,  x3,  [x21, #32]\n"
      "ldp x4,  x5,  [x21, #48]\n"
      "ldp x6,  x7,  [x21, #64]\n"
      "ldp x8,  x9,  [x21, #80]\n"
      "ldp x10, x11, [x21, #96]\n"
      "ldp x12, x13, [x21, #112]\n"
      "ldp x14, x15, [x21, #128]\n"
      "ldr x16, [x21, #144]\n"
      "ldr x17, [x21, #152]\n"
      "ldr x19, [x21, #168]\n"
      "ldr x20, [x21, #176]\n"
      "ldp x22, x23, [x21, #192]\n"
      "ldp x24, x25, [x21, #208]\n"
      "ldp x26, x27, [x21, #224]\n"
      "ldr x28, [x21, #240]\n"
      "mov sp, x18\n"
      "ldr x21, [x21, #184]\n"
      "ldr x18, [sp], #16\n"
      "br x18\n"
      :
      : "r"(ctx)
      : "memory");

  __builtin_unreachable();
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
  const uintptr_t fault_address = ctx->far.x;
  const uintptr_t lazy_region_base = s_lazy_region_base.load(std::memory_order_acquire);
  const size_t lazy_region_size = s_lazy_region_size.load(std::memory_order_relaxed);

  if (lazy_region_size != 0 && fault_address >= lazy_region_base &&
      fault_address - lazy_region_base < lazy_region_size) [[unlikely]]
  {
    constexpr uintptr_t kCommitWindow = 64 * 1024;
    uintptr_t window_start = fault_address & ~(kCommitWindow - 1);
    if (window_start < lazy_region_base)
      window_start = lazy_region_base;
    uintptr_t window_end = window_start + kCommitWindow;
    if (window_end > lazy_region_base + lazy_region_size)
      window_end = lazy_region_base + lazy_region_size;

    void* batch_addr = reinterpret_cast<void*>(window_start);
    const Result batch_result = svcMapPhysicalMemory(batch_addr, window_end - window_start);
    if (R_SUCCEEDED(batch_result))
      RestoreContextAndJump(ctx);

    void* single_addr = reinterpret_cast<void*>(fault_address & ~uintptr_t{0xFFF});
    const Result page_result = svcMapPhysicalMemory(single_addr, 0x1000);
    if (R_SUCCEEDED(page_result))
      RestoreContextAndJump(ctx);
  }

  SContext sctx;
  for (int i = 0; i < 29; i++)
    sctx.regs[i] = ctx->cpu_gprs[i].x;
  sctx.fp = ctx->fp.x;
  sctx.lr = ctx->lr.x;
  sctx.sp = ctx->sp.x;
  sctx.pc = ctx->pc.x;
  sctx.pstate = ctx->pstate;
  sctx.far = ctx->far.x;

  if (Core::System::GetInstance().GetJitInterface().HandleFault(fault_address, &sctx)) [[likely]]
  {
    ctx->pc.x = sctx.pc;
    for (int i = 0; i < 29; i++)
      ctx->cpu_gprs[i].x = sctx.regs[i];
    ctx->fp.x = sctx.fp;
    ctx->lr.x = sctx.lr;
    ctx->sp.x = sctx.sp;

    RestoreContextAndJump(ctx);
  }

  svcExitProcess();
  __builtin_unreachable();
}

void InstallExceptionHandler()
{
}

void UninstallExceptionHandler()
{
}

bool IsExceptionHandlerSupported()
{
  return true;
}

#else  // _M_GENERIC or unsupported platform

void InstallExceptionHandler()
{
}

void UninstallExceptionHandler()
{
}

bool IsExceptionHandlerSupported()
{
  return false;
}

#endif

}  // namespace EMM
