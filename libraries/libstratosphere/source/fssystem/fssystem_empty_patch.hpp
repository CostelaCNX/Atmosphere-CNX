/*
 * Copyright (c) Atmosphère-NX
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

namespace ams::fssystem::impl {

    /* An empty counter table does not make a CTR-Ex patch plain text. Its
     * indirect metadata still uses ordinary CTR with the section's upper IV.
     * With no patch data, every nonempty mapping must read the original NCA.
     * Keep this format check independent of program IDs and signature policy. */
    inline Result ValidateEmptyAesCtrExPatch(const NcaFsHeaderReader &header, s64 storage_size, bool has_original) {
        const auto &patch = header.GetPatchInfo();
        R_UNLESS(header.GetEncryptionType() == NcaFsHeader::EncryptionType::AesCtrEx ||
                 header.GetEncryptionType() == NcaFsHeader::EncryptionType::AesCtrExSkipLayerHash,
                 fs::ResultInvalidNcaFsHeaderEncryptionType());
        /* E2: patch metadata may itself be integrity-checked even when there
         * are no counter entries. OpenStorageImpl must still create the normal
         * verified patch metadata storage; this gate never bypasses hashes. */
        R_UNLESS(!header.ExistsSparseLayer(), fs::ResultInvalidNcaFsHeader());
        R_UNLESS(has_original, fs::ResultInvalidNcaFsHeader());
        R_UNLESS(patch.aes_ctr_ex_size == 0, fs::ResultInvalidNcaPatchInfoAesCtrExSize());
        R_UNLESS(patch.indirect_offset == 0, fs::ResultInvalidNcaPatchInfoAesCtrExOffset());
        R_UNLESS(patch.indirect_size > 0 && util::IsAligned(static_cast<s64>(patch.indirect_size), NcaHeader::XtsBlockSize),
                 fs::ResultInvalidNcaPatchInfoIndirectSize());
        /* Comparisons avoid attacker-controlled signed offset addition. */
        R_UNLESS(storage_size >= 0 && patch.indirect_size <= storage_size &&
                 patch.aes_ctr_ex_offset >= patch.indirect_size && patch.aes_ctr_ex_offset <= storage_size,
                 fs::ResultNcaBaseStorageOutOfRangeB());

        BucketTree::Header counter_header, indirect_header;
        std::memcpy(std::addressof(counter_header), patch.aes_ctr_ex_header, sizeof(counter_header));
        std::memcpy(std::addressof(indirect_header), patch.indirect_header, sizeof(indirect_header));
        R_TRY(counter_header.Verify());
        R_TRY(indirect_header.Verify());
        R_UNLESS(counter_header.entry_count == 0 && indirect_header.entry_count > 0, fs::ResultInvalidBucketTreeEntryCount());
        const s64 node_size = IndirectStorage::QueryNodeStorageSize(indirect_header.entry_count);
        const s64 entry_size = IndirectStorage::QueryEntryStorageSize(indirect_header.entry_count);
        R_UNLESS(node_size >= 0 && entry_size >= 0 && node_size <= patch.indirect_size &&
                 entry_size <= patch.indirect_size - node_size, fs::ResultInvalidNcaIndirectStorageOutOfRange());
        R_SUCCEED();
    }

}
