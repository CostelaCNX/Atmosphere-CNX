// SPDX-License-Identifier: MIT
#ifndef NEXUS_VSD_PROTOCOL_H
#define NEXUS_VSD_PROTOCOL_H
#include <stdint.h>
#define NEXUS_VSD_MAGIC 0x31445356u
#define NEXUS_VSD_VERSION 1u
#define NEXUS_VSD_QUERY 70
#define NEXUS_VSD_ARM 71
#define NEXUS_VSD_BEGIN_LOAD 72
#define NEXUS_VSD_CANCEL 73
#define NEXUS_VSD_LOADER_SEEN 1u
#define NEXUS_VSD_ARMED 2u
#define NEXUS_VSD_ACTIVE 4u
// Capability bits in Status.reserved (ABI remains protocol v1).
#define NEXUS_VSD_CAP_USB_ROOT 1u
typedef struct NexusVsdStatus {
    uint32_t magic, version, flags, reserved;
    uint64_t serial, generation;
} NexusVsdStatus;
#endif
