#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "state.h"

struct NdsSavestateIdentity {
    std::string build_id;
    std::string rom_sha1;
};

struct NdsBusMemorySnapshot {
    std::vector<uint8_t> main_ram;
    std::vector<uint8_t> itcm;
    std::vector<uint8_t> dtcm;
    std::vector<uint8_t> shared_wram;
    std::vector<uint8_t> arm7_wram;
    std::vector<uint8_t> arm9_bios;
    std::vector<uint8_t> arm7_bios;

    std::vector<uint8_t> main_ram_written;
    std::vector<uint8_t> itcm_written;
    std::vector<uint8_t> dtcm_written;
    std::vector<uint8_t> shared_wram_written;
    std::vector<uint8_t> arm7_wram_written;

    std::vector<uint32_t> main_ram_generation;
    std::vector<uint32_t> itcm_generation;
    std::vector<uint32_t> dtcm_generation;
    std::vector<uint32_t> shared_wram_generation;
    std::vector<uint32_t> arm7_wram_generation;
};

struct NdsSchedulerSaveState {
    ArmCpuState cpu[2]{};
    uint32_t crs[2][NDS_RUNTIME_CALL_STACK_CAPACITY]{};
    uint32_t crs_depth[2]{};
    uint32_t deferred_cycles[2]{};
    uint64_t cycles[2]{};
    uint64_t system_timestamp = 0;
    uint8_t started[2]{};
    uint8_t terminal_halted[2]{};
};

struct NdsCp15SaveState {
    Cp15State visible{};
    uint32_t timing_generation = 0;
    uint32_t mpu_region[8]{};
    uint32_t cache_cfg[8]{};
    uint32_t access_perm[8]{};
};

struct NdsRuntimeSaveState {
    uint64_t insn_count[2]{};
    uint64_t force_tier3_misses = 0;
    uint32_t active_cpu = 0;
    uint32_t force_tier3 = 0;
};

struct NdsIoDmaSaveState {
    uint32_t src = 0;
    uint32_t dst = 0;
    uint32_t cnt = 0;
    uint32_t cur_src = 0;
    uint32_t cur_dst = 0;
    uint32_t remaining = 0;
    int32_t src_inc = 0;
    int32_t dst_inc = 0;
    uint16_t burst_index = 0;
    uint8_t start_mode = 0;
    uint8_t running = 0;
    uint8_t in_progress = 0;
    uint8_t burst_start = 0;
};

struct NdsIoTimerSaveState {
    uint16_t reload = 0;
    uint16_t counter = 0;
    uint16_t ctrl = 0;
    uint64_t accum = 0;
};

// Architectural state owned by io.cpp. This deliberately excludes host
// resources, cartridge/firmware contents, trace rings, and device objects.
// Those require their own sections; copying their raw structs would persist
// pointers, callbacks, mutexes, and host-thread state.
struct NdsIoCoreSaveState {
    uint16_t ipcsync_out[2]{};
    uint8_t postflg[2]{};

    uint16_t dispstat[2]{};
    uint16_t vcount = 0;
    uint16_t next_vcount = 0;
    uint8_t next_vcount_valid = 0;
    uint8_t vcount_match[2]{};
    uint8_t in_vblank = 0;
    uint64_t display_last = 0;

    uint32_t ime[2]{};
    uint32_t ie[2]{};
    uint32_t irq_flags[2]{};
    uint8_t haltcnt[2]{};
    uint8_t cpu_halted[2]{};
    uint64_t halt_entry_cycle[2]{};

    uint32_t fifo[2][16]{};
    uint8_t fifo_count[2]{};
    uint8_t fifo_head[2]{};
    uint16_t fifocnt[2]{};
    uint32_t fifo_lastrx[2]{};

    NdsIoDmaSaveState dma[2][4]{};
    uint64_t dma_entry_cycle[2]{};
    uint8_t gxfifo_stall = 0;

    NdsIoTimerSaveState timer[2][4]{};
    uint64_t timer_last[2]{};

    uint16_t divcnt = 0;
    uint32_t div_numer[2]{};
    uint32_t div_denom[2]{};
    uint32_t div_quot[2]{};
    uint32_t div_rem[2]{};
    uint64_t div_deadline = UINT64_MAX;
    uint16_t sqrtcnt = 0;
    uint32_t sqrt_value[2]{};
    uint32_t sqrt_result = 0;
    uint64_t sqrt_deadline = UINT64_MAX;

    uint16_t exmemcnt[2]{};
    uint16_t powercontrol7 = 0;
    uint32_t keyinput = 0;
    uint16_t keycnt[2]{};
    uint16_t rcnt = 0;
    uint8_t wramcnt = 0;
    uint16_t wifiwaitcnt = 0;
    uint32_t biosprot = 0;

    uint8_t pm_index = 0;
    uint8_t pm_regs[8]{};
    uint8_t pm_masks[8]{};
    uint8_t pm_hold = 0;
    uint8_t powered_off = 0;
    uint8_t tsc_ctrl = 0;
    uint16_t tsc_conv = 0;
    int32_t tsc_datapos = 0;
    uint16_t tsc_x = 0;
    uint16_t tsc_y = 0;

    uint8_t io_mem[0x2000]{};
};

// Gamecard, SPI-firmware, and RTC state owned by io.cpp. Paths and host file
// handles are deliberately absent. The persistence-detached flags are host
// policy state: a historical load may restore guest-visible flash contents,
// but must not silently replace a newer canonical battery file.
struct NdsIoPeripheralSaveState {
    uint32_t romctrl = 0;
    uint32_t card_transfer_pos = 0;
    uint32_t card_transfer_len = 0;
    uint64_t card_deadline = UINT64_MAX;
    uint8_t card_end_event = 0;
    uint8_t card_irq_cpu = 0;
    uint8_t card_command[8]{};
    uint8_t card_command_mode = 0;
    uint32_t card_data_mode = 0;
    std::vector<uint8_t> card_response;
    uint32_t key1_schedule[0x412]{};
    uint32_t card_chip_id = 0;
    uint8_t key1_available = 0;
    uint8_t card_has_ir = 0;

    uint16_t auxspicnt = 0;
    uint8_t auxspi_data = 0;
    uint8_t auxspi_hold = 0;
    uint32_t auxspi_pos = 0;
    uint64_t auxspi_deadline = UINT64_MAX;
    uint8_t cart_ir_cmd = 0;

    uint8_t backup_type = 0;
    uint32_t backup_size = 0;
    std::vector<uint8_t> backup_data;
    uint8_t backup_cmd = 0;
    uint8_t backup_status = 0;
    uint32_t backup_addr = 0;
    uint8_t backup_dirty = 0;
    uint8_t backup_persistence_detached = 0;

    uint16_t spicnt = 0;
    uint8_t spi_response = 0;
    uint64_t spi_deadline = UINT64_MAX;
    std::vector<uint8_t> firmware_data;
    uint8_t firmware_dirty = 0;
    uint8_t firmware_persistence_detached = 0;
    uint8_t firmware_hold = 0;
    uint8_t firmware_cmd = 0;
    uint8_t firmware_status = 0;
    uint32_t firmware_addr = 0;
    uint32_t firmware_data_pos = 0;

    uint16_t rtc_io = 0;
    uint8_t rtc_input = 0;
    uint32_t rtc_inbit = 0;
    uint32_t rtc_inpos = 0;
    uint8_t rtc_output[8]{};
    uint32_t rtc_outbit = 0;
    uint32_t rtc_outpos = 0;
    uint8_t rtc_cmd = 0;
    uint8_t rtc_datetime[7]{};
    uint8_t rtc_status1 = 0;
    uint8_t rtc_status2 = 0;
    uint8_t rtc_alarm1[3]{};
    uint8_t rtc_alarm2[3]{};
    uint8_t rtc_clock_adjust = 0;
    uint8_t rtc_free = 0;
    uint8_t rtc_irq_flag = 0;
    uint32_t rtc_clock_count = 0;
    uint64_t rtc_processed_ticks = 0;
};

bool bus_savestate_export(NdsBusMemorySnapshot* out);
bool bus_savestate_import(const NdsBusMemorySnapshot& in, std::string* error);

bool scheduler_savestate_export(NdsSchedulerSaveState* out);
bool scheduler_savestate_import(const NdsSchedulerSaveState& in,
                                std::string* error);

void cp15_savestate_export(NdsCp15SaveState* out);
bool cp15_savestate_import(const NdsCp15SaveState& in, std::string* error);

void runtime_savestate_export(NdsRuntimeSaveState* out);
bool runtime_savestate_import(const NdsRuntimeSaveState& in,
                              std::string* error);
void runtime_savestate_invalidate_host_caches();

bool io_savestate_export(NdsIoCoreSaveState* out);
bool io_savestate_validate(const NdsIoCoreSaveState& in, std::string* error);
bool io_savestate_import(const NdsIoCoreSaveState& in, std::string* error);

bool io_peripheral_savestate_export(NdsIoPeripheralSaveState* out);
bool io_peripheral_savestate_validate(const NdsIoPeripheralSaveState& in,
                                      std::string* error);
bool io_peripheral_savestate_import(const NdsIoPeripheralSaveState& in,
                                    std::string* error);

using NdsSavestateEligibilityHook = bool (*)(void* context,
                                             std::string* error);
// Called after the scheduler-quiescence check and before any export or apply.
// The current Wi-Fi query surface reports backend/thread activity but not the
// guest's association or local-MP connection state, so it cannot faithfully
// implement the connected-only policy. A future network owner must register a
// hook backed by that actual state; treating "backend enabled" as connected
// would incorrectly disable offline states.
void nds_savestate_set_eligibility_hook(NdsSavestateEligibilityHook hook,
                                        void* context);

bool nds_savestate_save_core(const std::string& path,
                             const NdsSavestateIdentity& identity,
                             std::string* error);
bool nds_savestate_load_core(const std::string& path,
                             const NdsSavestateIdentity& expected_identity,
                             std::string* error);
