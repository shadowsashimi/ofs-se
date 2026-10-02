#include "OFS_CrashHandler.h"

#ifdef WIN32
#include "OFS_Util.h"
#include "OFS_FileLogging.h"

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <ctime>
#include <atomic>
#include <thread>

#pragma comment(lib, "dbghelp.lib")

namespace
{

// A hang is only interesting once it is clearly not just a slow frame. Opening
// a multi-gigabyte video can legitimately block the main thread for seconds.
constexpr uint64_t HangThresholdMs = 15000;
constexpr int MaxHangReports = 3;

std::atomic<uint64_t> LastHeartbeat{ 0 };
std::atomic<int> HangReports{ 0 };
std::atomic<bool> WatchdogRunning{ false };
HANDLE MainThread = nullptr;

FILE* openReport(const char* prefix) noexcept
{
    // Timestamped so a second occurrence does not overwrite the first.
    char name[80];
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s_%%Y%%m%%d_%%H%%M%%S.txt", prefix);

    const auto now = time(nullptr);
    struct tm local;
    localtime_s(&local, &now);
    strftime(name, sizeof(name), pattern, &local);

    auto path = Util::Prefpath(name);
    return fopen(path.c_str(), "w");
}

void writeLine(FILE* f, const char* fmt, ...) noexcept
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    stbsp_vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if(f) {
        fputs(buffer, f);
        fputc('\n', f);
        fflush(f);
    }
    LOGF_ERROR("%s", buffer);
}

const char* exceptionName(DWORD code) noexcept
{
    switch(code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case 0xE06D7363: return "C++ exception (likely thrown through noexcept)";
        default: return "unknown";
    }
}

void writeStack(FILE* f, HANDLE thread, CONTEXT* context) noexcept
{
    HANDLE process = GetCurrentProcess();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    if(!SymInitialize(process, nullptr, TRUE)) {
        writeLine(f, "SymInitialize failed (%lu), no symbol names available", GetLastError());
    }

    STACKFRAME64 frame;
    ZeroMemory(&frame, sizeof(frame));
    frame.AddrPC.Offset = context->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    // Space for the symbol name is allocated past the end of the struct.
    char symbolBuffer[sizeof(SYMBOL_INFO) + 512];
    auto symbol = (SYMBOL_INFO*)symbolBuffer;
    ZeroMemory(symbolBuffer, sizeof(symbolBuffer));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;

    writeLine(f, "--- stack ---");
    for(int depth = 0; depth < 48; depth += 1) {
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, context,
               nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if(frame.AddrPC.Offset == 0) break;

        DWORD64 displacement = 0;
        const char* name = "<unknown>";
        if(SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
            name = symbol->Name;
        }

        IMAGEHLP_LINE64 line;
        ZeroMemory(&line, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if(SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line)) {
            writeLine(f, "[%02d] %s  (%s:%lu)", depth, name, line.FileName, line.LineNumber);
        }
        else {
            writeLine(f, "[%02d] %s  (0x%llx)", depth, name, (unsigned long long)frame.AddrPC.Offset);
        }
    }

    SymCleanup(process);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info) noexcept
{
    FILE* f = openReport("crash");
    const auto code = info->ExceptionRecord->ExceptionCode;
    writeLine(f, "OFS-SE crashed: %s (0x%08lx)", exceptionName(code), (unsigned long)code);
    writeLine(f, "at address 0x%llx",
        (unsigned long long)info->ExceptionRecord->ExceptionAddress);

    if(code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        const auto op = info->ExceptionRecord->ExceptionInformation[0];
        writeLine(f, "%s address 0x%llx",
            op == 0 ? "reading" : (op == 1 ? "writing" : "executing"),
            (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    }

    writeStack(f, GetCurrentThread(), info->ContextRecord);
    if(f) fclose(f);
    return EXCEPTION_EXECUTE_HANDLER;
}

void terminateHandler() noexcept
{
    FILE* f = openReport("crash");
    writeLine(f, "OFS-SE called std::terminate");
    // A throw through a noexcept frame lands here. Rethrowing recovers the type.
    if(auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        }
        catch(const std::exception& e) {
            writeLine(f, "uncaught std::exception: %s", e.what());
        }
        catch(...) {
            writeLine(f, "uncaught exception of unknown type");
        }
    }

    CONTEXT context;
    RtlCaptureContext(&context);
    writeStack(f, GetCurrentThread(), &context);
    if(f) fclose(f);
    _exit(3);
}

void reportHang(uint64_t stalledForMs) noexcept
{
    if(MainThread == nullptr) return;

    // The main thread has to hold still to walk its stack.
    if(SuspendThread(MainThread) == (DWORD)-1) return;

    CONTEXT context;
    ZeroMemory(&context, sizeof(context));
    context.ContextFlags = CONTEXT_FULL;

    FILE* f = openReport("hang");
    if(GetThreadContext(MainThread, &context)) {
        writeLine(f, "OFS-SE main thread has not made progress for %llums",
            (unsigned long long)stalledForMs);
        writeLine(f, "This is where it is stuck:");
        writeStack(f, MainThread, &context);
    }
    else {
        writeLine(f, "GetThreadContext failed (%lu)", GetLastError());
    }
    if(f) fclose(f);

    ResumeThread(MainThread);
}

void watchdogLoop() noexcept
{
    for(;;) {
        Sleep(1000);
        const uint64_t last = LastHeartbeat.load();
        if(last == 0) continue;

        const uint64_t now = GetTickCount64();
        if(now < last + HangThresholdMs) {
            // Progress resumed, so a later stall is worth reporting again.
            if(now < last + 1000) HangReports.store(0);
            continue;
        }
        if(HangReports.load() >= MaxHangReports) continue;

        HangReports.fetch_add(1);
        reportHang(now - last);
    }
}

}

void OFS_CrashHandler::Install() noexcept
{
    SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(terminateHandler);

    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
        GetCurrentProcess(), &MainThread,
        0, FALSE, DUPLICATE_SAME_ACCESS);

    // Start the clock now rather than at the first frame, so that a hang during
    // startup is reported instead of waiting forever for a heartbeat.
    LastHeartbeat.store(GetTickCount64());

    if(!WatchdogRunning.exchange(true)) {
        std::thread(watchdogLoop).detach();
    }
}

void OFS_CrashHandler::Heartbeat() noexcept
{
    LastHeartbeat.store(GetTickCount64());
}

#else
void OFS_CrashHandler::Install() noexcept {}
void OFS_CrashHandler::Heartbeat() noexcept {}
#endif
