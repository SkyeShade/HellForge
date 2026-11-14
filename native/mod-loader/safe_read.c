/* SafeRead runs thousands of times a frame for the loader's readers and every mod's HfApi.read. ReadProcessMemory is a
 * system call each time, so a read of up to 256 bytes is copied in place by HfSafeCopy instead; a fault inside the
 * copy becomes the failure return, as ReadProcessMemory fails on memory it cannot read. Larger reads keep the system
 * call. Included by hellforge_modloader.c after perf_fixes.c. */

#define SAFE_READ_COPY_MAX 256

int HfSafeCopy(void* to, const void* from, size_t size);
extern const char HfSafeCopyStart[], HfSafeCopyEnd[], HfSafeCopyFault[];
/* A byte loop with no stack use: the fault handler moves rip to HfSafeCopyFault, which returns 1. */
__asm__(".text\n.p2align 4\nHfSafeCopy:\n  testq %r8, %r8\n  jz 2f\nHfSafeCopyStart:\n1:\n  movb (%rdx), %al\n  movb %al, (%rcx)\n"
        "  incq %rdx\n  incq %rcx\n  decq %r8\n  jnz 1b\nHfSafeCopyEnd:\n2:\n  xorl %eax, %eax\n  ret\n"
        "HfSafeCopyFault:\n  movl $1, %eax\n  ret\n");

static volatile LONG g_safeCopyOn, g_safeCopyFaults;
static INIT_ONCE g_safeCopyOnce = INIT_ONCE_STATIC_INIT;

/* 1 for an exception raised inside HfSafeCopy. The loader's other first-in-chain handlers pass such an exception on,
 * so SafeReadFault turns it into a failed read whatever order they were added in. */
static int SafeReadOwns(DWORD64 rip)
{
    return rip >= (DWORD64)(uintptr_t)HfSafeCopyStart && rip < (DWORD64)(uintptr_t)HfSafeCopyEnd;
}

/* A guard page's fault has already taken the page's guard away: it is put back, so the stack it belongs to still grows. */
static LONG CALLBACK SafeReadFault(EXCEPTION_POINTERS* info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode, old;
    if (!SafeReadOwns(info->ContextRecord->Rip)) return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != STATUS_GUARD_PAGE_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) return EXCEPTION_CONTINUE_SEARCH;
    if (code == STATUS_GUARD_PAGE_VIOLATION && info->ExceptionRecord->NumberParameters > 1)
    {
        MEMORY_BASIC_INFORMATION m;
        void* page = (void*)(info->ExceptionRecord->ExceptionInformation[1] & ~(ULONG_PTR)0xFFF);
        if (VirtualQuery(page, &m, sizeof m) && m.State == MEM_COMMIT) VirtualProtect(page, 0x1000, m.Protect | PAGE_GUARD, &old);
    }
    info->ContextRecord->Rip = (DWORD64)(uintptr_t)HfSafeCopyFault;
    InterlockedIncrement(&g_safeCopyFaults);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static BOOL CALLBACK SafeReadInstall(PINIT_ONCE once, void* parameter, void** context)
{
    if (PerfFixOn("safe-read") && AddVectoredExceptionHandler(1, SafeReadFault) != NULL) InterlockedExchange(&g_safeCopyOn, 1);
    return TRUE;
}

static int SafeRead(uintptr_t address, void* buffer, size_t size)
{
    InitOnceExecuteOnce(&g_safeCopyOnce, SafeReadInstall, NULL, NULL);
    if (g_safeCopyOn && size <= SAFE_READ_COPY_MAX)
        return address < 0x10000 || address > 0x7FFFFFFEFFFFull - size ? 1 : HfSafeCopy(buffer, (const void*)address, size);
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, buffer, size, &read) && read == size ? 0 : 1;
}
