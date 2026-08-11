/*
    ndsrecomp shim replacing melonDS's NDS.h for the vendored GPU3D engine
    and, since 2026-08, the vendored Wifi device model + net glue.

    The vendored GPU3D.cpp/GPU3D_Soft.cpp/Wifi.cpp/WifiAP.cpp and the net/
    subdirectory's .cpp files are unmodified (Wifi/net: modified per the two
    tracked patches in patches/) melonDS 1.0rc sources; this header
    supplies, by the same names, exactly the slice of the melonDS::NDS
    interface those units consume:
      - GPU3D (surveyed 2026-07-16): GPU member, ARM9Timestamp/
        ARM9ClockShift, SetIRQ/ClearIRQ, CheckDMAs, GXFIFOStall/
        GXFIFOUnstall, IRQ_GXFIFO.
      - Wifi/net (surveyed 2026-08-10, wired to bus.cpp/scheduler.cpp
        2026-08-10, see docs/adr-melonds-wifi-vendoring.md §3): SPI member
        (NDS.SPI.GetFirmware()), a single-slot
        RegisterEventFuncs/UnregisterEventFuncs/ScheduleEvent/CancelEvent
        (Wifi only ever registers one event func, Event_Wifi -> USTimer),
        ConsoleType, UserData, IRQ_Wifi.
    The method bodies live in runner/src/gpu3d.cpp (GPU3D) and
    runner/src/wifi_net.cpp (Wifi/net); both forward to the runner's own
    device models.

    As an interface derived from melonDS this file is distributed under the
    same terms as the vendored sources: GPL-3.0-or-later (see GPU3D.h).
    Copyright 2016-2024 melonDS team; shim adaptation 2026 ndsrecomp.
*/

#ifndef NDS_H
#define NDS_H

#include <initializer_list>

#include "types.h"
#include "GPU.h"
#include "SPI.h"

namespace melonDS
{

// Bit indices into IE/IF, matching the melonDS NDS.h enum (GPU3D only ever
// names IRQ_GXFIFO; the value must stay 21 = GBATEK "Geometry Command FIFO").
// IRQ_Wifi = 24 matches melonDS's own IRQ enum position (counting from
// IRQ_VBlank=0 through IRQ_SPI=23) and GBATEK's documented ARM7 IRQ bit 24 =
// Wifi -- confirmed against the real NDS.h at the pinned tag, not assumed.
enum
{
    IRQ_GXFIFO = 21,
    IRQ_Wifi = 24,
};

// Matching melonDS's own event-table position (LCD=0, SPU=1, Wifi=2); the
// numeric value is opaque to this single-slot shim (only one event source
// -- Wifi -- ever uses it), kept for readability against the real source.
enum
{
    Event_Wifi = 2,
};

typedef void (*EventFunc)(void* that, u32 param);
#define MakeEventThunk(class, func) [](void* that, u32 param) { static_cast<class*>(that)->func(param); }

class NDS
{
public:
    NDS() noexcept : GPU(*this) {}

    melonDS::GPU GPU;
    melonDS::SPI SPI;

    // ARM9 time in the 2x CPU clock domain; the bridge synchronizes this with
    // the runner's ARM9 timestamp before letting the geometry engine run.
    u64 ARM9Timestamp = 0;
    u32 ARM9ClockShift = 1;

    // 0 = DS, 1 = DSi. The runner does not model DSi hardware; Wifi.cpp only
    // consults this to gate a DSi-only console-type branch (Wifi.cpp:191),
    // which a fixed 0 correctly keeps closed.
    u32 ConsoleType = 0;

    // Opaque context melonDS's frontends thread through Platform::MP_*/
    // Net_*; the runner's shim convention (see gpu3d.cpp) uses file-scope
    // statics instead of a threaded context pointer, so this stays nullptr.
    void* UserData = nullptr;

    void SetIRQ(u32 cpu, u32 irq);
    void ClearIRQ(u32 cpu, u32 irq);
    void CheckDMAs(u32 cpu, u32 mode);
    void GXFIFOStall();
    void GXFIFOUnstall();

    // Single-slot scheduler shim. Wifi::Wifi()/~Wifi() (Wifi.cpp:92-104)
    // register/unregister exactly one event func (USTimer) under exactly
    // one id (Event_Wifi); Wifi::ScheduleTimer/UpdatePowerOn only ever
    // schedule/cancel that same id with FuncID 0. A generic multi-event
    // dispatch table (real melonDS's SchedList[Event_MAX]) is therefore
    // unnecessary here -- this stores one deadline/callback pair.
    //
    // Wired into the runner's live scheduler via runner/src/wifi_net.cpp's
    // nds_wifi_next_event_time()/nds_wifi_run_events(): every rendezvous the
    // scheduler folds Event_Wifi's deadline into its global "next event"
    // minimum (scheduler.cpp:82) and drains due events by calling
    // RunPendingEvent() in a loop gated on CurrentSystemTimestamp, exactly
    // mirroring the real melonDS NDS::RunSystem/ScheduleEvent contract
    // (ndsref/third_party/melonDS/src/NDS.cpp:1159-1177) but simplified to
    // the one event id Wifi ever uses.
    void RegisterEventFuncs(u32 id, void* that,
                             const std::initializer_list<EventFunc>& funcs);
    void UnregisterEventFuncs(u32 id);
    void ScheduleEvent(u32 id, bool periodic, s32 delay, u32 funcid,
                        u32 param);
    void CancelEvent(u32 id);

    // Bridge-facing accessors driving the live scheduler integration
    // (runner/src/wifi_net.cpp's nds_wifi_next_event_time/run_events).
    [[nodiscard]] bool HasPendingEvent() const noexcept { return EventScheduled; }
    [[nodiscard]] u64 PendingEventTime() const noexcept { return EventTimestamp; }
    void RunPendingEvent();

    // The system-cycle ("1x"/ARM7-clock) timestamp at the point of the
    // current Wifi-driven call, in the SAME units Wifi::ScheduleTimer's
    // `delay` is computed in (Wifi.cpp:319-329: 33513982 Hz-based). The
    // bridge sets this immediately before any call into the vendored Wifi
    // object that might trigger Wifi::ScheduleTimer (a register write, a
    // SetPowerCnt POWCNT2 change, or nds_wifi_run_events' own rendezvous),
    // and RunPendingEvent()/ScheduleEvent's non-periodic branch consult it
    // directly. Real melonDS's NDS::ScheduleEvent instead derives this from
    // (ARM9Timestamp>>ARM9ClockShift) or ARM7Timestamp depending on CurCPU
    // (NDS.cpp:1159-1177); a separate field is used here rather than
    // reusing ARM9Timestamp because that field is independently owned by
    // the GPU3D bridge (gpu3d.cpp) in the 2x/ARM9 clock domain, and because
    // a CurCPU branch is unnecessary: nds_wifi_address gates every access
    // to cpu==7, so Wifi's event scheduling is unconditionally ARM7-owned
    // and the ARM9 branch of real melonDS's ScheduleEvent is unreachable
    // for Event_Wifi in practice.
    u64 CurrentSystemTimestamp = 0;

private:
    static constexpr u32 kNoEvent = 0xFFFFFFFFu;

    u32 EventID = kNoEvent;
    void* EventThat = nullptr;
    EventFunc EventCallback = nullptr;
    u64 EventTimestamp = 0;
    u32 EventParam = 0;
    // Mirrors real melonDS's SchedListMask bit for this one id: scheduling
    // an already-scheduled event is a bug (real NDS::ScheduleEvent logs and
    // bails rather than silently overwriting); CancelEvent/RunPendingEvent
    // clear it.
    bool EventScheduled = false;
};

}

#endif // NDS_H
