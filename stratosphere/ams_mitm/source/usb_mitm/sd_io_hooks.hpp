// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "sd_io_core.hpp"
namespace nexus_sd_io {
extern thread_local Origin current_origin;
void EnterRequest();
void LeaveRequest();
void Snapshot(NexusSdIoSnapshot* out,uint64_t generation);
class Scope {
    Origin previous;
    bool request;
public:
    explicit Scope(Origin value):previous(current_origin),request(value==Origin::UsbRequest&&previous!=value) {
        current_origin=value;if(request)EnterRequest();
    }
    ~Scope(){if(request)LeaveRequest();current_origin=previous;}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
}
