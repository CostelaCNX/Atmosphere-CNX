// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stratosphere.hpp>
namespace ams::mitm::usb_storage {
// Missing index succeeds with nullptr. An enabled but invalid/offline source
// returns an error, never an unrelated native-data fallback. No USB startup.
Result OpenUsbModRomfs(std::shared_ptr<ams::fs::IStorage> *out,ncm::ProgramId title);
Result SnapshotUsbModBase(std::shared_ptr<ams::fs::IStorage> *base);
Result ApplyUsbCachedExefs(std::shared_ptr<ams::fs::fsa::IFileSystem> *code,ncm::ProgramId title);
}
