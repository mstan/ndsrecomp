/*
    ndsrecomp shim replacing melonDS's Platform.h for the vendored GPU3D
    engine and Savestate.cpp.

    Declares, with melonDS's exact signatures, the host services those
    unmodified translation units consume (surveyed 2026-07-16): Log/LogLevel,
    Thread_Create/Wait/Free and Semaphore_Create/Free/Reset/Wait/Post.
    Implementations live in runner/src/gpu3d.cpp. The primitives are real
    (std::thread / mutex+condvar) so every vendored code path is sound, but
    the runner never enables SoftRenderer threading: deterministic,
    oracle-comparable execution requires the single-threaded render path.

    As an interface derived from melonDS this file is distributed under the
    same terms as the vendored sources: GPL-3.0-or-later (see GPU3D.h).
    Copyright 2016-2024 melonDS team; shim adaptation 2026 ndsrecomp.
*/

#ifndef PLATFORM_H
#define PLATFORM_H

#include <functional>

#include "types.h"

namespace melonDS::Platform
{

enum LogLevel
{
    Debug,
    Info,
    Warn,
    Error,
};

void Log(LogLevel level, const char* fmt, ...);

struct Thread;
Thread* Thread_Create(std::function<void()> func);
void Thread_Free(Thread* thread);
void Thread_Wait(Thread* thread);

struct Semaphore;
Semaphore* Semaphore_Create();
void Semaphore_Free(Semaphore* sema);
void Semaphore_Reset(Semaphore* sema);
void Semaphore_Wait(Semaphore* sema);
void Semaphore_Post(Semaphore* sema, int count = 1);

}

#endif // PLATFORM_H
