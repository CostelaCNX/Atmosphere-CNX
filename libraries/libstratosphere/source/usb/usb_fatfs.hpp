/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 or later.
 */
#pragma once

#include <stratosphere/usb/usb_mass_storage.hpp>
#include <stratosphere/usb/usb_volume.hpp>

namespace ams::usb::fatfs {

    Result Attach(mass_storage::Device *device, const volume::Partition &partition);
    void Detach();
    void DiscardCleanSectors();

}
