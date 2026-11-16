/*
 * Copyright (C) Switch-OC-Suite
 *
 * Copyright (c) 2023 hanai3Bi
 *
 * Copyright (c) Souldbminer, Lightos_ and Horizon OC Contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "../oc_common.hpp"
#include "pcv_common.hpp"
#include "pcv_asm.hpp"

namespace ams::ldr::hoc::pcv::erista {

    constexpr cvb_entry_t CpuCvbTableDefault[] = {
        // CPU_PLL_CVB_TABLE_ODN
        {  204000,  {721094}, {                         } },
        {  306000,  {754040}, {                         } },
        {  408000,  {786986}, {                         } },
        {  510000,  {819932}, {                         } },
        {  612000,  {852878}, {                         } },
        {  714000,  {885824}, {                         } },
        {  816000,  {918770}, {                         } },
        {  918000,  {951716}, {                         } },
        { 1020000,  {984662}, { -2875621,  358099, -8585} },
        { 1122000, {1017608}, {   -52225,  104159, -2816} },
        { 1224000, {1050554}, {  1076868,    8356,  -727} },
        { 1326000, {1083500}, {  2208191,  -84659,  1240} },
        { 1428000, {1116446}, {  2519460, -105063,  1611} },
        { 1581000, {1130000}, {  2889664, -122173,  1834} },
        { 1683000, {1168000}, {  5100873, -279186,  4747} },
        { 1785000, {1227500}, {  5100873, -279186,  4747} },
        {                                                 },
    };

    constexpr u32 CpuVoltOfficial = 1227;
    constexpr u32 CpuVminOfficial = 825;
    constexpr u32 CpuTune0Low     = 0xFFEAD0FF;

    constexpr u32 CpuVoltL4T = 1257'000;

    static const u32 cpuVoltDvfsPattern[] = { 1227, 1000, 100, 1000, 0 };
    static_assert(sizeof(cpuVoltDvfsPattern) == 0x14, "Invalid cpuVoltDvfsPattern size");

    static const u32 cpuVoltageThermalPattern[] = { 950, 1132, 0, 950, 1227, 0, 825, 1227, 15000, 825, 1170, 60000, 825, 1132, 80000 };
    static_assert(sizeof(cpuVoltageThermalPattern) == 0x3c, "Invalid cpuVoltageThermalPattern size");

    constexpr u32 GpuClkPllLimit  = 2'600'000;
    constexpr u32 GpuClkPllMax    = 921'600'000;
    constexpr u32 GpuVminOfficial = 810;

    constexpr u16 CpuMinVolts[] = { 950, 850, 825, 810 };

    static const u32 gpuVoltDvfsPattern[] = { 810, 1150, 1000, 100, 1000, 10, };
    static_assert(sizeof(gpuVoltDvfsPattern) == (sizeof(u32) * 6), "Invalid gpuVoltDvfsPattern");

    static const u32 gpuVoltThermalPattern[] = { 950, 1132, 0, 810, 1132, 15000, 810, 1132, 30000, 810, 1132, 50000, 810, 1132, 70000, 810, 1132, 105000 };
    static_assert(sizeof(gpuVoltThermalPattern) == 0x48, "Invalid gpuVoltageThermalPattern size");

    /* GPU Max Clock asm Pattern:
        *
        * MOV W11, #0x1000 MOV (wide immediate)                0x1000                              0xB (11)
        *  sf | opc |                 | hw  |                   imm16                        |      Rd
        * #31 |30 29|28 27 26 25 24 23|22 21|20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5 |4  3  2  1  0
        *   0 | 1 0 | 1  0  0  1  0  1| 0  0| 0  0  0  1  0  0  0  0  0  0  0  0  0  0  0  0 |0  1  0  1  1
        *
        * MOVK W11, #0xE, LSL#16     <shift>16                    0xE                              0xB (11)
        *  sf | opc |                 | hw  |                   imm16                        |      Rd
        * #31 |30 29|28 27 26 25 24 23|22 21|20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5 |4  3  2  1  0
        *   0 | 1 1 | 1  0  0  1  0  1| 0  1| 0  0  0  0  0  0  0  0  0  0  0  0  1  1  1  0 |0  1  0  1  1
        */
    inline constexpr u32 GpuAsmPattern[] = { 0x52820000, 0x72A001C0 };

    inline bool GpuMaxClockPatternFn(u32 *ptr32) {
        return asm_compare_no_rd(*ptr32, GpuAsmPattern[0]);
    };

    constexpr cvb_entry_t GpuCvbTableDefault[] = {
        // NA_FREQ_CVB_TABLE
        {  76800, {}, {  814294, 8144, -940, 808, -21583, 226, } },
        { 153600, {}, {  856185, 8144, -940, 808, -21583, 226, } },
        { 230400, {}, {  898077, 8144, -940, 808, -21583, 226, } },
        { 307200, {}, {  939968, 8144, -940, 808, -21583, 226, } },
        { 384000, {}, {  981860, 8144, -940, 808, -21583, 226, } },
        { 460800, {}, { 1023751, 8144, -940, 808, -21583, 226, } },
        { 537600, {}, { 1065642, 8144, -940, 808, -21583, 226, } },
        { 614400, {}, { 1107534, 8144, -940, 808, -21583, 226, } },
        { 691200, {}, { 1149425, 8144, -940, 808, -21583, 226, } },
        { 768000, {}, { 1191317, 8144, -940, 808, -21583, 226, } },
        { 844800, {}, { 1233208, 8144, -940, 808, -21583, 226, } },
        { 921600, {}, { 1275100, 8144, -940, 808, -21583, 226, } },
        {                                                        },
    };

    constexpr u32 EmcListDefault[]   = { 40800, 68000, 102000, 204000, 408000, 665600, 800000, 1065600, 1331200, 1600000, };
    constexpr u32 EmcListSizeDefault = std::size(EmcListDefault);
    constexpr u32 EmcListEndDefault  = EmcListSizeDefault - 1;

    constexpr u32 MemVoltHOS      = 1125'000;
    constexpr u32 EmcClkPllmLimit = 1866'000'000;

    constexpr u32 MTC_TABLE_REV        = 7;
    constexpr u32 MtcTableCountDefault = 10;

    constexpr size_t MtcFullTableSize  = sizeof(EristaMtcTable) * MtcTableCountDefault;
    constexpr u32 MtcFullTableCount    = 3;

    /* These dramids were copied from Hekate -- see /bdk/mem/sdram.h */
    enum DramId {
        ICOSA_4GB_SAMSUNG_K4F6E304HB_MGCH        = 0,
        ICOSA_4GB_HYNIX_H9HCNNNBPUMLHR_NLE       = 1,
        ICOSA_4GB_MICRON_MT53B512M32D2NP_062_WTC = 2,
        ICOSA_6GB_SAMSUNG_K4FHE3D4HM_MGCH        = 4,
        ICOSA_8GB_SAMSUNG_K4FBE3D4HM_MGXX        = 7,
    };

    enum MtcTableIndex {
        T210SdevEmcDvfsTableS4gb01 = 0, /* HB-MGCH, WT:C */
        T210SdevEmcDvfsTableS6gb01 = 1, /* HM-MGCH */
        T210SdevEmcDvfsTableH4gb01 = 2, /* HR-NLE  */
        MtcTableIndex_Invalid      = 3,
    };

    struct MtcDramIndex {
        DramId dramId;
        MtcTableIndex index;
    };

    /* TODO: Test 6gb and 8gb. */
    const inline MtcDramIndex mtcIndexTable[] = {
        { ICOSA_4GB_SAMSUNG_K4F6E304HB_MGCH,        T210SdevEmcDvfsTableS4gb01, },
        { ICOSA_4GB_MICRON_MT53B512M32D2NP_062_WTC, T210SdevEmcDvfsTableS4gb01, },
        { ICOSA_6GB_SAMSUNG_K4FHE3D4HM_MGCH,        T210SdevEmcDvfsTableS6gb01, },
        { ICOSA_8GB_SAMSUNG_K4FBE3D4HM_MGXX,        T210SdevEmcDvfsTableS6gb01, },
        { ICOSA_4GB_HYNIX_H9HCNNNBPUMLHR_NLE,       T210SdevEmcDvfsTableH4gb01, },
    };

    constexpr u32 MtcBrAsm   = 0xD61F0140;
    constexpr u32 MtcMovAsm  = 0x52800148;
    constexpr u32 MtcAdrpAsm = 0xD0000081;
    constexpr u32 MtcBlIns = 0x97ffae64;
    constexpr u32 MtcAddAsm = 0x91131821;

    ALWAYS_INLINE bool MemMtcGetGetTablePatternFn(u32 *ptr) {
        /* This builds an address that gets returned, so the register must be x0 by convention. */
        return AsmCompareAddNoImm12(*ptr, MtcAddAsm);
    }

    void Patch(uintptr_t mapped_nso, size_t nso_size);

}
