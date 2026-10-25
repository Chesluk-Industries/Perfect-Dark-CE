#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "platform.h"

#define CRASH_LOG_FNAME "pd.crash.log"
#define CRASH_MAX_MSG 8192
#define CRASH_MAX_SYM 256
#define CRASH_MAX_FRAMES 32
#define CRASH_MSG(...) \
	if (msglen < CRASH_MAX_MSG) msglen += snprintf(msg + msglen, CRASH_MAX_MSG - msglen, __VA_ARGS__)

#if defined(PLATFORM_WIN32)

#include <windows.h>
#include <dbghelp.h>
#include <inttypes.h>
#include <excpt.h>

// NOTE: game builds with gcc, which means we have no PDBs for the windows version
// this means that you generally won't get any symbol names in the main executable

static LPTOP_LEVEL_EXCEPTION_FILTER prevExFilter;
static DWORD crashMainThreadId;

static void *crashGetModuleBase(const void *addr)
{
	HMODULE h = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, addr, &h);
	return (void *)h;
}

// MOD: crash logs: the image base the exe was linked at, so addresses can be
// turned into the ones addr2line expects even with ASLR
static uintptr_t crashGetPreferredBase(void *modbase)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)modbase;
	const IMAGE_NT_HEADERS *nt;

	if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return (uintptr_t)modbase;
	}

	nt = (const IMAGE_NT_HEADERS *)((const u8 *)modbase + dos->e_lfanew);

	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return (uintptr_t)modbase;
	}

	return (uintptr_t)nt->OptionalHeader.ImageBase;
}

// MOD: hang logs - thread: the thread whose stack it is (the hang watchdog
// walks the main thread's)
static void crashStackTrace(char *msg, PEXCEPTION_POINTERS exinfo, HANDLE thread)
{
	CONTEXT context = *exinfo->ContextRecord;
	void *mainbase = crashGetModuleBase(crashInit);
	const uintptr_t mainpref = crashGetPreferredBase(mainbase);
	uintptr_t mainaddrs[CRASH_MAX_FRAMES + 1];
	s32 nummainaddrs = 0;
	HANDLE process = GetCurrentProcess();

	SymSetOptions(SymGetOptions() | SYMOPT_DEBUG | SYMOPT_LOAD_LINES);
	SymInitialize(process, NULL, TRUE);

	STACKFRAME64 stackframe;
	memset(&stackframe, 0, sizeof(stackframe));

	DWORD image;
#ifdef PLATFORM_X86
	image = IMAGE_FILE_MACHINE_I386;
	stackframe.AddrPC.Offset = context.Eip;
	stackframe.AddrPC.Mode = AddrModeFlat;
	stackframe.AddrFrame.Offset = context.Ebp;
	stackframe.AddrFrame.Mode = AddrModeFlat;
	stackframe.AddrStack.Offset = context.Esp;
	stackframe.AddrStack.Mode = AddrModeFlat;
#elif defined(PLATFORM_X86_64)
	image = IMAGE_FILE_MACHINE_AMD64;
	stackframe.AddrPC.Offset = context.Rip;
	stackframe.AddrPC.Mode = AddrModeFlat;
	stackframe.AddrFrame.Offset = context.Rsp;
	stackframe.AddrFrame.Mode = AddrModeFlat;
	stackframe.AddrStack.Offset = context.Rsp;
	stackframe.AddrStack.Mode = AddrModeFlat;
#else
	snprintf(msg, CRASH_MAX_MSG, "no stack trace available on this arch\n");
	return;
#endif

	DWORD disp = 0;
	DWORD64 disp64 = 0;
	IMAGEHLP_LINE64 line;
	DWORD msglen = 0;

	CRASH_MSG("EXCEPTION: 0x%08lx", exinfo->ExceptionRecord->ExceptionCode);
	switch (exinfo->ExceptionRecord->ExceptionCode) {
	case EXCEPTION_ACCESS_VIOLATION:
		CRASH_MSG(" (access violation)");
		if (exinfo->ExceptionRecord->NumberParameters >= 2) {
			const ULONG_PTR kind = exinfo->ExceptionRecord->ExceptionInformation[0];
			CRASH_MSG(": %s address %p", kind == 0 ? "reading" : (kind == 1 ? "writing" : "executing"),
				(void *)exinfo->ExceptionRecord->ExceptionInformation[1]);
		}
		break;
	case EXCEPTION_STACK_OVERFLOW: CRASH_MSG(" (stack overflow)"); break;
	case EXCEPTION_INT_DIVIDE_BY_ZERO: CRASH_MSG(" (integer divide by zero)"); break;
	case EXCEPTION_ILLEGAL_INSTRUCTION: CRASH_MSG(" (illegal instruction)"); break;
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: CRASH_MSG(" (array bounds exceeded)"); break;
	case EXCEPTION_IN_PAGE_ERROR: CRASH_MSG(" (in-page error)"); break;
	case 0xE0000001: CRASH_MSG(" (abort)"); break;
	case 0xE0000002: CRASH_MSG(" (hang: the game stopped responding; this is where the main thread was)"); break;
	default: break;
	}
	CRASH_MSG("\n");
	CRASH_MSG("PC: %p", exinfo->ExceptionRecord->ExceptionAddress);
	if (SymGetLineFromAddr64(process, (uintptr_t)exinfo->ExceptionRecord->ExceptionAddress, &disp, &line)) {
		CRASH_MSG(": %s:%lu+%lu", line.FileName, line.LineNumber, disp);
	}
	CRASH_MSG("\nMODULE: [%p]\n", crashGetModuleBase(exinfo->ExceptionRecord->ExceptionAddress));
	CRASH_MSG("MAIN MODULE: [%p] (linked at %p)\n", mainbase, (void *)mainpref);
#ifdef PLATFORM_X86_64
	CRASH_MSG("\nREGISTERS:\n");
	CRASH_MSG("RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n", context.Rax, context.Rbx, context.Rcx, context.Rdx);
	CRASH_MSG("RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n", context.Rsi, context.Rdi, context.Rbp, context.Rsp);
	CRASH_MSG("R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n", context.R8, context.R9, context.R10, context.R11);
	CRASH_MSG("R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n", context.R12, context.R13, context.R14, context.R15);
#endif
	CRASH_MSG("\nBACKTRACE (module-relative offsets in brackets):\n");

	char symbuf[sizeof(SYMBOL_INFO) + CRASH_MAX_SYM * sizeof(TCHAR)];
	PSYMBOL_INFO sym = (PSYMBOL_INFO)symbuf;
	sym->SizeOfStruct = sizeof(*sym);
	sym->MaxNameLen = CRASH_MAX_SYM;

	s32 i;
	for (i = 0; i < CRASH_MAX_FRAMES; ++i) {
		const BOOL res = StackWalk64(image, process, thread, &stackframe, &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL);
		if (!res) {
			break;
		}

		CRASH_MSG("#%02d: %p", i, (void *)(uintptr_t)stackframe.AddrPC.Offset);

		{
			const uintptr_t modbase = (uintptr_t)crashGetModuleBase((void *)(uintptr_t)stackframe.AddrPC.Offset);
			const uintptr_t modofs = (uintptr_t)stackframe.AddrPC.Offset - modbase;
			char modname[MAX_PATH] = { 0 };
			const char *shortname = "?";

			if (modbase && GetModuleFileNameA((HMODULE)modbase, modname, sizeof(modname) - 1)) {
				const char *slash = strrchr(modname, '\\');
				shortname = slash ? slash + 1 : modname;
			}

			CRASH_MSG(" [%s+0x%llx]", shortname, (unsigned long long)modofs);

			// return addresses point after the call: -1 lands on the call itself
			if (modbase && modbase == (uintptr_t)mainbase && nummainaddrs < CRASH_MAX_FRAMES) {
				mainaddrs[nummainaddrs++] = mainpref + modofs - (i > 0 ? 1 : 0);
			}
		}

		if (SymFromAddr(process, stackframe.AddrPC.Offset, &disp64, sym)) {
			CRASH_MSG(": %s+%llu", sym->Name, disp64);
		}

		if(SymGetLineFromAddr64(process, stackframe.AddrPC.Offset, &disp, &line)) {
			CRASH_MSG(" (%s:%lu+%lu)", line.FileName, line.LineNumber, disp);
		}

		CRASH_MSG("\n");
	}

	if (i <= 1) {
		CRASH_MSG("no information\n");
	} else if (i == CRASH_MAX_FRAMES) {
		CRASH_MSG("...\n");
	}

	// gcc builds have no PDB, so the names come from addr2line (MSYS2) on
	// this exact exe, which keeps its debug info (RelWithDebInfo)
	if (nummainaddrs > 0) {
		CRASH_MSG("\nTO GET FUNCTION NAMES AND LINES, run in the MSYS2 MINGW64 shell next to this exe:\naddr2line -f -C -i -p -e pd.x86_64.exe");
		for (s32 j = 0; j < nummainaddrs; j++) {
			CRASH_MSG(" 0x%llx", (unsigned long long)mainaddrs[j]);
		}
		CRASH_MSG("\n");
	}

	SymCleanup(process);
}

static char crashMsgBuf[CRASH_MAX_MSG + 1];


static long __stdcall crashHandler(PEXCEPTION_POINTERS exinfo)
{
	// static: there may be little stack left (stack overflow)
	char *msg = crashMsgBuf;
	static s32 inHandler = 0;

	if (inHandler) {
		return EXCEPTION_CONTINUE_SEARCH; // crashed while handling a crash
	}

	inHandler = 1;
	memset(crashMsgBuf, 0, sizeof(crashMsgBuf));

	if (IsDebuggerPresent()) {
		if (prevExFilter) {
			return prevExFilter(exinfo);
		}
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	sysLogPrintf(LOG_ERROR, "FATAL: Crashed: PC=%p CODE=0x%08lx", exinfo->ExceptionRecord->ExceptionAddress, exinfo->ExceptionRecord->ExceptionCode);

	fflush(stderr);
	fflush(stdout);

	// MOD: crash logs: a short log first, with nothing that can fail
	{
		static char early[1024];
		void *pc = exinfo->ExceptionRecord->ExceptionAddress;
		void *modbase = crashGetModuleBase(pc);
		void *mainbase = crashGetModuleBase(crashInit);
		char modname[MAX_PATH] = { 0 };
		const char *shortname = "?";

		if (modbase && GetModuleFileNameA((HMODULE)modbase, modname, sizeof(modname) - 1)) {
			const char *slash = strrchr(modname, '\\');
			shortname = slash ? slash + 1 : modname;
		}

		snprintf(early, sizeof(early),
			"Crashed: PC=%p [%s+0x%llx] CODE=0x%08lx, thread %lu (%s)\n"
			"addr2line -f -C -i -p -e pd.x86_64.exe 0x%llx\n",
			pc, shortname, (unsigned long long)((uintptr_t)pc - (uintptr_t)modbase),
			exinfo->ExceptionRecord->ExceptionCode, GetCurrentThreadId(),
			GetCurrentThreadId() == crashMainThreadId ? "main thread" : "not the main thread",
			(unsigned long long)(modbase == mainbase && modbase
				? crashGetPreferredBase(mainbase) + ((uintptr_t)pc - (uintptr_t)modbase) : 0));
		sysWriteCrashLogEarly(early);
	}

	crashStackTrace(msg, exinfo, GetCurrentThread());

	// MOD: crash logs: always written (pd.crash.log + a dated copy), with or without --log
	sysWriteCrashLog(msg);

	sysFatalError("Crash! A crash log was saved as pd.crash.log.\n\n%s", msg);

	return EXCEPTION_CONTINUE_EXECUTION;
}

/**
 * MOD: hang logs - a watchdog thread. When the main thread hasn't finished a
 * frame for CRASH_HANG_SECONDS (an endless loop: the game freezes with no
 * crash), it writes a crash log with the main thread's stack and the last
 * log lines, then leaves the game as it is. Once per hang.
 */
#define CRASH_HANG_SECONDS 10

static volatile LONG crashFrames = 0;
static char crashHangBuf[CRASH_MAX_MSG + 1];

void crashHeartbeat(void)
{
	InterlockedIncrement(&crashFrames);
}

static void crashReportHang(void)
{
	HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, crashMainThreadId);
	CONTEXT context;
	EXCEPTION_RECORD record;
	EXCEPTION_POINTERS pointers;
	s32 ok = 0;

	if (thread == NULL) {
		return;
	}

	memset(crashHangBuf, 0, sizeof(crashHangBuf));
	memset(&context, 0, sizeof(context));
	memset(&record, 0, sizeof(record));

	if (SuspendThread(thread) != (DWORD)-1) {
		context.ContextFlags = CONTEXT_FULL;

		if (GetThreadContext(thread, &context)) {
			record.ExceptionCode = 0xE0000002;
#ifdef PLATFORM_X86_64
			record.ExceptionAddress = (void *)(uintptr_t)context.Rip;
#elif defined(PLATFORM_X86)
			record.ExceptionAddress = (void *)(uintptr_t)context.Eip;
#endif
			pointers.ExceptionRecord = &record;
			pointers.ContextRecord = &context;
			crashStackTrace(crashHangBuf, &pointers, thread);
			ok = 1;
		}

		ResumeThread(thread);
	}

	CloseHandle(thread);

	if (ok) {
		sysLogPrintf(LOG_ERROR, "HANG: the game hasn't finished a frame for %d s, a crash log was saved (pd.crash.log)", CRASH_HANG_SECONDS);
		sysWriteCrashLog(crashHangBuf);
	}
}

static DWORD WINAPI crashWatchdog(LPVOID arg)
{
	LONG last = crashFrames;
	s32 stuck = 0;
	s32 reported = 0;

	for (;;) {
		LONG now;

		Sleep(1000);
		now = crashFrames;

		if (now != last || IsDebuggerPresent()) {
			last = now;
			stuck = 0;
			reported = 0;
			continue;
		}

		// (now == 0: no frame yet, still starting up)
		if (now != 0 && !reported && ++stuck >= CRASH_HANG_SECONDS) {
			reported = 1;
			crashReportHang();
		}
	}

	return 0;
}

// abort() (failed asserts, C++ terminate) doesn't go through the exception filter
static void crashAbortHandler(int sig)
{
	CONTEXT context;
	EXCEPTION_RECORD record;
	EXCEPTION_POINTERS pointers;

	signal(SIGABRT, SIG_DFL);
	memset(&context, 0, sizeof(context));
	memset(&record, 0, sizeof(record));
	RtlCaptureContext(&context);
	record.ExceptionCode = 0xE0000001;
#ifdef PLATFORM_X86_64
	record.ExceptionAddress = (void *)(uintptr_t)context.Rip;
#elif defined(PLATFORM_X86)
	record.ExceptionAddress = (void *)(uintptr_t)context.Eip;
#endif
	pointers.ExceptionRecord = &record;
	pointers.ContextRecord = &context;
	crashHandler(&pointers);
}

#elif defined(PLATFORM_LINUX)

#include <ucontext.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <ctype.h>
#include <dlfcn.h>
#include <sys/fcntl.h>

static struct sigaction prevSigAction;

static s32 crashIsDebuggerPresent(void)
{
	static s32 result = -1;

	if (result >= 0) {
		return result;
	}

	char buf[4096] = { 0 };

	int fd = open("/proc/self/status", O_RDONLY);
	if (fd < 0) {
		result = 0;
		return 0;
	}

	int rx = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (rx <= 0) {
		result = 0;
		return 0;
	}

	buf[rx] = 0;

	char *str = strstr(buf, "TracerPid:");
	if (!str) {
		result = 0;
		return 0;
	}

	str += 10;

	while (*str && !isdigit(*str)) {
		if (*str == '\n') {
			result = 0;
			return 0;
		}
		++str;
	}

	result = (atoi(str) != 0);
	return result;
}

static void *crashGetModuleBase(const void *addr)
{
	Dl_info info;
	if (dladdr(addr, &info)) {
		return info.dli_fbase;
	}
	return NULL;
}

static void crashStackTrace(char *msg, s32 sig, void *pc)
{
	u32 msglen = 0;
	void *frames[CRASH_MAX_FRAMES] = { NULL };

	const s32 nframes = backtrace(frames, CRASH_MAX_FRAMES);
	if (nframes <= 0) {
		CRASH_MSG("no information\n");
		return;
	}

	char **strings = backtrace_symbols(frames, nframes);

	CRASH_MSG("SIGNAL: %d\n", sig);
	CRASH_MSG("PC: ");
	if (pc) {
		CRASH_MSG("%p\n", pc);
	} else if (strings) {
		CRASH_MSG("%s\n", strings[0]);
	} else {
		CRASH_MSG("%p\n", frames[0]);
	}

	CRASH_MSG("MODULE: %p\n", crashGetModuleBase(frames[0]));
	CRASH_MSG("MAIN MODULE: %p\n", crashGetModuleBase(crashInit));
	CRASH_MSG("\nBACKTRACE:\n");

	s32 i;
	for (i = 0; i < nframes; ++i) {
		CRASH_MSG("#%02d: ", i);
		if (strings && strings[i]) {
			CRASH_MSG("%s\n", strings[i]);
		} else {
			CRASH_MSG("%p\n", frames[i]);
		}
	}

	if (i == CRASH_MAX_FRAMES) {
		CRASH_MSG("...\n");
	} else if (i <= 1) {
		CRASH_MSG("no information\n");
	}

	free(strings);
}

static void crashHandler(s32 sig, siginfo_t *siginfo, void *ctx)
{
	char msg[CRASH_MAX_MSG + 1] = { 0 };

	if (crashIsDebuggerPresent()) {
		return;
	}

	void *pc = NULL;
	if (ctx) {
		ucontext_t *ucontext = (ucontext_t *)ctx;
#ifdef PLATFORM_X86
		pc = (void *)ucontext->uc_mcontext.gregs[REG_EIP];
#elif defined(PLATFORM_X86_64)
		pc = (void *)ucontext->uc_mcontext.gregs[REG_RIP];
#elif defined(PLATFORM_ARM) && defined(PLATFORM_64BIT)
		pc = (void *)ucontext->uc_mcontext.pc;
#elif defined(PLATFORM_ARM)
		pc = (void *)ucontext->uc_mcontext.arm_pc;
#endif
	}

	sysLogPrintf(LOG_ERROR, "FATAL: Crashed: PC=%p SIGNAL=%d", pc, sig);

	fflush(stderr);
	fflush(stdout);

	crashStackTrace(msg, sig, pc);

	// MOD: crash logs
	sysWriteCrashLog(msg);

	sysFatalError("Crash!\n\n%s", msg);
}

#endif

s32 g_CrashEnabled = 0;

static char crashMsg[1024];

void crashInit(void)
{
#ifdef PLATFORM_WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS);
	crashMainThreadId = GetCurrentThreadId();
	prevExFilter = SetUnhandledExceptionFilter(crashHandler);
	// MOD: crash logs: keep some stack for the handler after a stack overflow,
	// and catch abort() too
	{
		typedef BOOL (WINAPI *STACKGUARANTEEFN)(PULONG);
		STACKGUARANTEEFN fn = (STACKGUARANTEEFN)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadStackGuarantee");
		ULONG guarantee = 64 * 1024;
		if (fn) {
			fn(&guarantee);
		}
	}
	signal(SIGABRT, crashAbortHandler);
	g_CrashEnabled = 1;

	// MOD: hang logs
	{
		HANDLE watchdog = CreateThread(NULL, 256 * 1024, crashWatchdog, NULL, 0, NULL);

		if (watchdog) {
			CloseHandle(watchdog);
		}
	}
#elif defined(PLATFORM_LINUX)
	struct sigaction sigact = { 0 };
	sigact.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigact.sa_sigaction = crashHandler;
	sigaction(SIGSEGV, &sigact, &prevSigAction);
	sigaction(SIGABRT, &sigact, &prevSigAction);
	sigaction(SIGBUS,  &sigact, &prevSigAction);
	sigaction(SIGILL,  &sigact, &prevSigAction);
	g_CrashEnabled = 1;
#endif
}

void crashReinstall(void)
{
#ifdef PLATFORM_WIN32
	LPTOP_LEVEL_EXCEPTION_FILTER cur;

	if (!g_CrashEnabled) {
		return;
	}

	cur = SetUnhandledExceptionFilter(crashHandler);

	if (cur != crashHandler) {
		sysLogPrintf(LOG_WARNING, "crash handler had been replaced (by %p), put back", (void *)cur);
	}
#endif
}

void crashShutdown(void)
{
	if (!g_CrashEnabled) {
		return;
	}
#ifdef PLATFORM_WIN32
	if (prevExFilter) {
		SetUnhandledExceptionFilter(prevExFilter);
	}
#elif defined(PLATFORM_LINUX)
	sigaction(SIGSEGV, &prevSigAction, NULL);
	sigaction(SIGABRT, &prevSigAction, NULL);
	sigaction(SIGBUS,  &prevSigAction, NULL);
	sigaction(SIGILL,  &prevSigAction, NULL);
#endif
	g_CrashEnabled = 0;
}

#ifndef PLATFORM_WIN32
void crashHeartbeat(void)
{
	// MOD: hang logs (Windows only)
}
#endif

void crashCreateThread(void)
{

}

void crashSetMessage(char *string)
{

}

void crashReset(void)
{

}

void crashAppendChar(char c)
{

}
