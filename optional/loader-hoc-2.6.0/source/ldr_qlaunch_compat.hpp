/*
 * Copyright (c) Atmosphere-NX contributors, CostelaCNX and NEXUS contributors.
 * GPL-2.0-only. Pattern/replacement words from CostelaCNX/Atmosphere-CNX
 * d912e82a409c9e1b43750f2167b98fcef912cd18 (2026-10-09).
 * Narrow backport: no signature/verification or other program patches.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ams::ldr::qlaunch_compat {
    /* HOS version encoding, local to this patch: do not widen global firmware
     * support or silently opt future firmware into executable rewriting. */
    inline constexpr std::uint32_t Firmware2300 = 0x17000000;
    inline constexpr std::uint32_t Firmware2301 = 0x17000100;
    inline constexpr bool SupportsFirmware(std::uint32_t version) {
        return version == Firmware2300 || version == Firmware2301;
    }

    struct Patch {
        std::uint32_t words[4];
        std::uint32_t replacement;
    };

    inline constexpr Patch Patches[] = {
        {{0xF9400008, 0xF9412D08, 0xD63F0100, 0x35FFFDA0}, 0xF9410D08},
        {{0x5400092A, 0x4B0802CA, 0x528000EB, 0xF81C03BF}, 0x5280002A},
        {{0x528A0009, 0x72A00289, 0xA907ABE9, 0x941C3719}, 0x52A00409},
    };

    /* Caller restricts this to qlaunch's main executable on 23.0.0/23.0.1, after its
     * original segment hashes pass and before user IPS overrides. The three
     * changes are a unit: missing/ambiguous/already mixed patterns write
     * nothing. Search only aligned instructions in the bounded text segment.
     * This deliberately fails closed on a changed firmware layout. */
    inline bool Apply(std::uint8_t *text, std::size_t size) {
        if (text == nullptr || size < sizeof(Patches[0].words)) return false;
        std::size_t positions[3]{};
        for (std::size_t p = 0; p < 3; ++p) {
            unsigned matches = 0;
            for (std::size_t i = 0; i <= size - sizeof(Patches[p].words); i += 4) {
                if (std::memcmp(text + i, Patches[p].words, sizeof(Patches[p].words)) == 0) {
                    if (++matches > 1) return false;
                    positions[p] = i;
                }
            }
            if (matches != 1) return false;
        }
        for (std::size_t a = 0; a < 3; ++a) {
            for (std::size_t b = a + 1; b < 3; ++b) {
                if (positions[a] < positions[b] + 16 && positions[b] < positions[a] + 16) return false;
            }
        }
        for (std::size_t p = 0; p < 3; ++p) {
            std::memcpy(text + positions[p] + 4, &Patches[p].replacement, sizeof(Patches[p].replacement));
        }
        return true;
    }

    inline bool ApplyForFirmware(std::uint32_t version, std::uint8_t *text, std::size_t size) {
        return SupportsFirmware(version) && Apply(text, size);
    }
}
