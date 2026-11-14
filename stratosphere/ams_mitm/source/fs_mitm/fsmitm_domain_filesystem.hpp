/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stratosphere.hpp>
#include "../usb_mitm/usb_romfs_probe.hpp"

namespace ams::mitm::fs {
    Result WrapUsbControlFileSystemForMitm(std::shared_ptr<ams::fs::fsa::IFileSystem> *out,
        sf::cmif::DomainObjectId *out_id, std::shared_ptr<ams::fs::fsa::IFileSystem> base,
        std::shared_ptr<::Service> forward, ncm::ProgramId program_id);
    Result WrapUsbProgramFileSystemForMitm(std::shared_ptr<ams::fs::fsa::IFileSystem> *out,
        sf::cmif::DomainObjectId *out_id, std::shared_ptr<ams::fs::fsa::IFileSystem> base,
        std::shared_ptr<::Service> forward, ncm::ProgramId program_id);
    Result WrapUsbStorageForMitm(std::shared_ptr<ams::fs::IStorage> *out,
        sf::cmif::DomainObjectId *out_id, std::shared_ptr<ams::fs::IStorage> base,
        std::shared_ptr<::Service> forward, ncm::ProgramId program_id, usb_storage::LaunchPin pin);
}
