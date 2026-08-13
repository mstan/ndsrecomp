/*
    ndsrecomp shim replacing melonDS's Platform.h for the vendored GPU3D
    engine, Savestate.cpp, and (since 2026-08) the vendored Wifi device
    model + net glue (Wifi.cpp, WifiAP.cpp, and the net/ subdirectory).

    Declares, with melonDS's exact signatures, the host services those
    unmodified translation units consume:
      - GPU3D (surveyed 2026-07-16): Log/LogLevel, Thread_Create/Wait/Free,
        Semaphore_Create/Free/Reset/Wait/Post. Implemented in
        runner/src/gpu3d.cpp.
      - Wifi/net (surveyed 2026-08-10, see
        docs/adr-melonds-wifi-vendoring.md §3): Mutex_Create/Free/Lock/
        Unlock (PacketDispatcher's queue lock), the nine MP_* local-
        wireless functions (experimental localhost transport for same-
        machine multi-instance play), Net_SendPacket/
        Net_RecvPacket + SendPacketCallback (the WifiAP<->NetDriver
        bridge), and, only when NDS_ENABLE_PCAP_BACKEND is on,
        DynamicLibrary_Load/Unload/LoadFunction (Net_PCap's runtime
        wpcap.dll load). Implemented in runner/src/wifi_net.cpp.

    The threading primitives are real (std::thread / mutex+condvar) so
    every vendored code path is sound, but the runner never enables
    SoftRenderer threading or a live Wifi/Net backend from this vendoring
    pass: deterministic, oracle-comparable execution requires the
    single-threaded render path, and the Wifi/net device model is wired
    but intentionally not yet switched onto the live bus (see
    runner/src/wifi_net.cpp's header comment).

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

struct Mutex;
Mutex* Mutex_Create();
void Mutex_Free(Mutex* mutex);
void Mutex_Lock(Mutex* mutex);
void Mutex_Unlock(Mutex* mutex);

// Local-wireless (multiplayer/Download Play/NiFi) transport. The runner
// currently implements an experimental same-machine localhost transport
// for multi-instance play; LAN/across-machine play is not claimed.
void MP_Begin(void* userdata);
void MP_End(void* userdata);
int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata);
int MP_RecvPacket(u8* data, u64* timestamp, void* userdata);
int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata);
int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata);
int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata);
int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata);
u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata);

// Network comm interface (WifiAP::SendPacket/RecvPacket's frontend hook,
// WifiAP.cpp:304,371). Packet type: Ethernet (802.3).
int Net_SendPacket(u8* data, int len, void* userdata);
int Net_RecvPacket(u8* data, void* userdata);
using SendPacketCallback = std::function<void(const u8* data, int len)>;

#if defined(NDS_ENABLE_PCAP_BACKEND)
// Backs Net_PCap's runtime (not link-time) load of wpcap.dll/Packet.dll --
// only compiled when the off-by-default NDS_ENABLE_PCAP_BACKEND option is
// set (runner/CMakeLists.txt). See runner/src/wifi_net.cpp.
struct DynamicLibrary;
DynamicLibrary* DynamicLibrary_Load(const char* lib);
void DynamicLibrary_Unload(DynamicLibrary* lib);
void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name);
#endif

}

#endif // PLATFORM_H
