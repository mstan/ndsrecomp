// platform_oracle.cpp — headless melonDS Platform implementation for the
// nds_oracle shim.
//
// The melonDS core (src/CMakeLists.txt `core`) declares the Platform::*
// interface (src/Platform.h) but ships no implementation — every frontend
// provides its own. The qt_sdl frontend's Platform_Qt is Qt/SDL-bound; the
// oracle is headless, so this is a minimal POSIX/C-runtime backing:
//   * file I/O over the C stdio FILE* API,
//   * threads/mutex/semaphore over the C++ std threading library,
//   * everything the oracle does not exercise (Net/MP/Camera/Addon/RTC/save
//     persistence/dynamic libraries) stubbed to a safe no-op.
//
// Save/firmware writeback is intentionally a no-op: the oracle is a read-only
// reference, it must never mutate the dumps the native runtime hashes.

#include "Platform.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace melonDS::Platform
{

// ---------------------------------------------------------------- lifecycle --

void SignalStop(StopReason, void*) {}

// --------------------------------------------------------------------- files --
// FileHandle is opaque to the core; we back it directly with a FILE*.

static const char* modeString(FileMode mode)
{
    bool read  = mode & FileMode::Read;
    bool write = mode & FileMode::Write;
    bool text  = mode & FileMode::Text;
    bool append = mode & FileMode::Append;
    bool preserve = mode & FileMode::Preserve;

    // Map the melonDS FileMode flags onto fopen mode strings.
    if (append)            return text ? "a+t" : "a+b";
    if (read && write)     return (preserve ? (text ? "r+t" : "r+b")
                                            : (text ? "w+t" : "w+b"));
    if (write)             return preserve ? (text ? "r+t" : "r+b")
                                           : (text ? "wt"  : "wb");
    return text ? "rt" : "rb";
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if (mode & FileMode::NoCreate)
    {
        FILE* probe = fopen(path.c_str(), "rb");
        if (!probe) return nullptr;
        fclose(probe);
    }
    FILE* f = fopen(path.c_str(), modeString(mode));
    return reinterpret_cast<FileHandle*>(f);
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(path, mode);
}

std::string GetLocalFilePath(const std::string& filename) { return filename; }

static bool exists(const std::string& name)
{
    FILE* f = fopen(name.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
bool FileExists(const std::string& name)      { return exists(name); }
bool LocalFileExists(const std::string& name) { return exists(name); }

bool CheckFileWritable(const std::string& filepath)
{
    FILE* f = fopen(filepath.c_str(), "ab");
    if (!f) return false;
    fclose(f);
    return true;
}
bool CheckLocalFileWritable(const std::string& filepath) { return CheckFileWritable(filepath); }

bool CloseFile(FileHandle* file)
{
    return fclose(reinterpret_cast<FILE*>(file)) == 0;
}

bool IsEndOfFile(FileHandle* file)
{
    return feof(reinterpret_cast<FILE*>(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return fgets(str, count, reinterpret_cast<FILE*>(file)) != nullptr;
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int whence = SEEK_SET;
    switch (origin)
    {
        case FileSeekOrigin::Start:   whence = SEEK_SET; break;
        case FileSeekOrigin::Current: whence = SEEK_CUR; break;
        case FileSeekOrigin::End:     whence = SEEK_END; break;
    }
    return fseek(reinterpret_cast<FILE*>(file), (long)offset, whence) == 0;
}

void FileRewind(FileHandle* file) { rewind(reinterpret_cast<FILE*>(file)); }

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return fread(data, size, count, reinterpret_cast<FILE*>(file));
}

bool FileFlush(FileHandle* file)
{
    return fflush(reinterpret_cast<FILE*>(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return fwrite(data, size, count, reinterpret_cast<FILE*>(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vfprintf(reinterpret_cast<FILE*>(file), fmt, args);
    va_end(args);
    return ret < 0 ? 0 : (u64)ret;
}

u64 FileLength(FileHandle* file)
{
    FILE* f = reinterpret_cast<FILE*>(file);
    long pos = ftell(f);
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long len = ftell(f);
    fseek(f, pos, SEEK_SET);  // restore, as the contract requires
    return len < 0 ? 0 : (u64)len;
}

// ------------------------------------------------------------------- logging --

void Log(LogLevel level, const char* fmt, ...)
{
    // Keep the oracle quiet unless something is wrong; boot/IO chatter would
    // drown the diff harness. Warn/Error go to stderr.
    if (level < LogLevel::Warn) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

// ------------------------------------------------------------------ threads --

struct Thread { std::thread t; };

Thread* Thread_Create(std::function<void()> func)
{
    Thread* th = new Thread();
    th->t = std::thread(std::move(func));
    return th;
}
void Thread_Free(Thread* thread)
{
    if (thread->t.joinable()) thread->t.join();
    delete thread;
}
void Thread_Wait(Thread* thread)
{
    if (thread->t.joinable()) thread->t.join();
}

struct Semaphore
{
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};

Semaphore* Semaphore_Create() { return new Semaphore(); }
void Semaphore_Free(Semaphore* sema) { delete sema; }
void Semaphore_Reset(Semaphore* sema)
{
    std::lock_guard<std::mutex> lk(sema->m);
    sema->count = 0;
}
void Semaphore_Wait(Semaphore* sema)
{
    std::unique_lock<std::mutex> lk(sema->m);
    sema->cv.wait(lk, [&] { return sema->count > 0; });
    sema->count--;
}
bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(sema->m);
    if (timeout_ms <= 0)
    {
        if (sema->count <= 0) return false;
    }
    else if (!sema->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [&] { return sema->count > 0; }))
    {
        return false;
    }
    sema->count--;
    return true;
}
void Semaphore_Post(Semaphore* sema, int count)
{
    std::lock_guard<std::mutex> lk(sema->m);
    sema->count += count;
    sema->cv.notify_all();
}

struct Mutex { std::mutex m; };
Mutex* Mutex_Create() { return new Mutex(); }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { mutex->m.lock(); }
void Mutex_Unlock(Mutex* mutex) { mutex->m.unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return mutex->m.try_lock(); }

// --------------------------------------------------------------------- time --

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
u64 GetUSCount()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------- save / firmware writeback (no-op) ----
// The oracle is a read-only reference; never persist anything back to disk.

void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

// ----------------------------------------- local multiplayer (unused) -------

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

// ----------------------------------------------------- networking (unused) --

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

// --------------------------------------------------------- camera (unused) --

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}

// ---------------------------------------------------------- addons (unused) --

void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}

// ------------------------------------------------- dynamic libraries (none) --

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

} // namespace melonDS::Platform
