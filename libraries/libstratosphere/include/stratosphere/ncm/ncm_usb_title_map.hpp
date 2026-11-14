/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#pragma once

#include <stratosphere/ncm/ncm_content_id.hpp>
#include <stratosphere/ncm/ncm_program_id.hpp>

namespace ams::ncm {

    /*
     * The title map is deliberately separate from NCM's content-meta
     * databases.  It is a small, persistent association used only by the
     * custom USB launch path:
     *
     *     application ProgramId -> Host/USB content storage
     *
     * Tickets and content metadata remain owned by Horizon's normal storage
     * databases.  A missing or unreadable map is treated as an empty map so
     * the stock SD/NAND launch path remains the fallback.
     */
    bool IsUsbTitleMapped(ProgramId program_id);
    Result ResolveUsbTitleAlias(ProgramId *out_program_id, ProgramId alias_program_id);
    Result ListUsbTitles(ProgramId *out_program_ids, size_t max_program_ids, u32 *out_count);
    Result RegisterUsbTitle(ProgramId program_id);
    /* Persist the exact Meta, Control, and Program ContentIds observed in the
     * committed application metadata. This is separate from title_map.bin so
     * tickets and Horizon's normal content-meta database remain untouched. */
    Result RegisterUsbTitleContent(ProgramId program_id, const ContentId &meta_content_id, const ContentId &control_content_id, const ContentId &program_content_id);
    Result GetUsbTitleContentIds(ProgramId program_id, ContentId *out_meta_content_id, ContentId *out_control_content_id, ContentId *out_program_content_id);
    /* Resolve the ProgramId encoded by an exact, persistently mapped title
     * content row. This keeps NCM's GetProgramId path local to NCM/SD and
     * avoids a launch-time callback into the USB provider. */
    Result ResolveUsbContentProgramId(ProgramId *out_program_id, const ContentId &content_id);
    Result UnregisterUsbTitle(ProgramId program_id);

    /* Exact NCA locations, recorded only after a successful USB registration.
     * Includes Data/LegalInformation and additional program content, not just
     * the three IDs used by the title-level launch map. These calls use SD/RAM
     * only; none may call USB, NCM IPC, or ES from the metadata commit path. */
    Result EnsureUsbContentLocationsLoaded();
    Result RefreshUsbProgramIdCache(u64 media_generation);
    Result RegisterUsbContentLocation(ContentId content_id);
    Result UnregisterUsbContentLocation(ContentId content_id);
    bool IsUsbContentLocationRegistered(ContentId content_id);
    bool WasUsbContentRegisteredThisBoot(ContentId content_id);
    Result ListUsbContentLocations(ContentId *out, size_t capacity, size_t offset, size_t *out_count);

}
