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

bool nds_savestate_save_core(const std::string& path,
                             const NdsSavestateIdentity& identity,
                             std::string* error);
bool nds_savestate_load_core(const std::string& path,
                             const NdsSavestateIdentity& expected_identity,
                             std::string* error);
