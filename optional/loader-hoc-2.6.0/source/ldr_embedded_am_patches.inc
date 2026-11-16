/*
 * Copyright (c) Atmosphère-NX
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

/* Patch teardown function call to NOP. */
constexpr inline const EmbeddedPatchEntry AmDisableTeardownPatches_22_0_0[] = {
    { 0x5CC4C, "\x1F\x20\x03\xD5", 4 },
};

constexpr inline const EmbeddedPatch AmDisableTeardownPatches[] = {
    { ParseModuleId("B337F7C5AD53E55F3D920FCD394B4DE7C3B7D606"), util::size(AmDisableTeardownPatches_22_0_0), AmDisableTeardownPatches_22_0_0 }, /* 22.0.0 */
};

/* AM 22.5.0 converts the useful startup failure into the generic AM result
 * 0x4680 at four nested boundaries. Keep the launch failing, but preserve the
 * original Result long enough for the passive IApplicationAccessor observer
 * to record it. These are diagnostic-only, exact-build patches:
 *
 *   0x495B4: mov w21, #0x4680 -> mov w21, w28
 *   0x49628: mov w21, #0x4680 -> mov w21, w28
 *   0x700C0: mov w0, #0x4680  -> mov w0, w21
 *   0x87538: result remapping -> b 0x87554 (return the current w0)
 *
 * At 0x495B4 and 0x49628, w28 is the native NS failure saved immediately
 * after the failed virtual call. The observed R50 path can otherwise hide NS
 * descriptions 320/321 before either outer wrapper is reached.
 *
 * No success path, launch decision, content path, ticket or metadata is
 * changed. An unknown AM build ID is left untouched. */
constexpr inline const EmbeddedPatchEntry AmExposeUsbLaunchResultPatches_22_5_0[] = {
    { 0x495B4, "\xF5\x03\x1C\x2A", 4 },
    { 0x49628, "\xF5\x03\x1C\x2A", 4 },
    { 0x700C0, "\xE0\x03\x15\x2A", 4 },
    { 0x87538, "\x07\x00\x00\x14", 4 },
};

/* Original firmware bytes paired by index with the table above. R52 records
 * the final mapped bytes after both embedded and SD IPS patches have run. */
constexpr inline const u8 AmExposeUsbLaunchResultOriginals_22_5_0[][4] = {
    { 0x15, 0xD0, 0x88, 0x52 },
    { 0x15, 0xD0, 0x88, 0x52 },
    { 0x00, 0xD0, 0x88, 0x52 },
    { 0x08, 0x54, 0x00, 0x12 },
};
static_assert(util::size(AmExposeUsbLaunchResultOriginals_22_5_0) == util::size(AmExposeUsbLaunchResultPatches_22_5_0));

constexpr inline const EmbeddedPatch AmExposeUsbLaunchResultPatches[] = {
    { ParseModuleId("93C0B7CC723C922159DD6E1FF4BD2C7C1E08E736"), util::size(AmExposeUsbLaunchResultPatches_22_5_0), AmExposeUsbLaunchResultPatches_22_5_0 }, /* 22.5.0 */
};
