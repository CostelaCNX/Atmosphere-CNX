/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* NS 22.5.0 returns FS PathNotFound before it calls PGL for a USB-backed
 * title. Tag only the three pre-PGL failure exits so the existing passive AM
 * result trace can identify the exact failing firmware call in one run.
 *
 * The two replacement instructions preserve all branch decisions. They copy
 * the low twelve bits of the BL return address from x30 into w22, which is the
 * result register returned by this function. NSO mappings are page-aligned,
 * so the tags remain stable under ASLR:
 *
 *   0xF6C: stage A, call 0x13F838 returned at 0x13EF6C
 *   0xF8C: stage B, call 0x13D394 returned at 0x13EF8C
 *   0xFC0: stage C, call 0x09913C returned at 0x13EFC0
 *
 * Stage A's temporary success value is overwritten by the later PGL result,
 * and neither replacement changes w0, so every success/failure branch remains
 * native. The Loader also verifies both original instructions before applying
 * either replacement; an unknown or already-modified NS image is untouched. */
constexpr inline const EmbeddedPatchEntry NsPrePglStageTagPatches_22_5_0[] = {
    { 0x13EF6C, "\xD6\x2F\x00\x12", 4 }, /* and w22, w30, #0xfff */
    { 0x13F038, "\xD6\x2F\x00\x12", 4 }, /* and w22, w30, #0xfff */
};

constexpr inline const u8 NsPrePglStageTagOriginals_22_5_0[][4] = {
    { 0xF6, 0x03, 0x00, 0x2A }, /* mov w22, w0 */
    { 0xF6, 0x03, 0x00, 0x2A }, /* mov w22, w0 */
};
static_assert(util::size(NsPrePglStageTagOriginals_22_5_0) == util::size(NsPrePglStageTagPatches_22_5_0));

constexpr inline const EmbeddedPatch NsPrePglStageTagPatches[] = {
    { ParseModuleId("95DC926DF3081A6AB61AD08809B56C66C2C29F41"), util::size(NsPrePglStageTagPatches_22_5_0), NsPrePglStageTagPatches_22_5_0 }, /* 22.5.0 */
};
