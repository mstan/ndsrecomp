#pragma once

#include <cstdint>
#include <string>

// Direct boot: the machine state the firmware boot would have left behind,
// established directly so a cartridge can start without running the firmware.
//
// This is the ONE genuinely high-level step in the opt-in no-dump path
// (beads-yjp.15). It is deliberately kept out of io.cpp and behind an explicit
// machine interface so the exact sequence can be pinned by direct_boot_test
// against melonDS NDS::SetupDirectBoot + FirmwareMem::SetupDirectBoot, which is
// the behaviour we are matching and the only thing that makes this checkable.
//
// Writes go through the machine's normal bus path rather than poking RAM: the
// copied ARM9/ARM7 binaries must acquire write provenance, or Tier-3 refuses to
// interpret them and static bank validation refuses to match them.

struct NdsDirectBootMachine {
    virtual ~NdsDirectBootMachine() = default;
    // cpu: 0 = ARM9, 1 = ARM7.
    virtual void write16(int cpu, uint32_t addr, uint16_t value) = 0;
    virtual void write32(int cpu, uint32_t addr, uint32_t value) = 0;
    // ARM9 CP15. Selectors are the MCR encoding, matching runtime_coproc_write.
    virtual void cp15_write(uint32_t crn, uint32_t crm, uint32_t op2,
                            uint32_t value) = 0;
    // Entry state for one core: PC and LR and r12 all at `entry`.
    virtual void set_cpu_boot(int cpu, uint32_t entry, uint32_t sp,
                              uint32_t sp_irq, uint32_t sp_svc) = 0;
    // Shared WRAM assignment (WRAMCNT); direct boot uses 3.
    virtual void set_wramcnt(uint8_t value) = 0;
    // ARM7 BIOS protection register.
    virtual void set_arm7_bios_prot(uint32_t value) = 0;
    // Post-boot device latches: POSTFLG both cores, POWCNT9, RCNT, cart SPICNT,
    // SOUNDBIAS, WIFIWAITCNT. Grouped because they are one atomic "the firmware
    // already ran" statement, not independently meaningful knobs.
    virtual void set_post_boot_latches() = 0;
    // FreeBIOS only: games read the Nintendo logo out of the ARM9 BIOS for
    // DS<->GBA comm, and a reimplemented BIOS has no copy of it.
    virtual void copy_logo_into_arm9_bios(const uint8_t* logo, uint32_t size) = 0;
};

struct NdsDirectBootInputs {
    const uint8_t* rom = nullptr;
    uint32_t rom_size = 0;
    // Decrypted secure area, 0x800 bytes, or null when the cartridge has none.
    // Our ROM image stores it RE-encrypted (io.cpp card_reencrypt_secure_area_
    // if_needed), so the caller supplies the plaintext rather than this module
    // reaching into the cartridge's KEY1 state.
    const uint8_t* secure_area = nullptr;
    // Firmware image the guest would have read its user settings from.
    const uint8_t* firmware = nullptr;
    uint32_t firmware_size = 0;
    // False when the ARM9 BIOS is a reimplementation rather than a dump.
    bool arm9_bios_is_native = true;
};

// Applies the whole sequence. Returns false and fills `error` if the cartridge
// header or firmware is too small or self-inconsistent -- refusing is always
// better than booting a machine into a half-initialised state.
bool nds_direct_boot(NdsDirectBootMachine& machine,
                     const NdsDirectBootInputs& inputs, std::string* error);
