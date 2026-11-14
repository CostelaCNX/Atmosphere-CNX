/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::arp {

    /* Register the mapped USB title's control.nacp against the stock ARP
     * instance which AM/NS has already created. This is deliberately a
     * short-lived client operation: it never owns or replaces arp:r/arp:w.
     * The operation must be invoked by the worker below, never from an
     * appletAE forwarding callback. */
    Result RegisterUsbApplicationControlProperty(u64 *out_instance_id, ncm::ProgramId application_id);

    /* Start the post-initialization worker and enqueue a title for its
     * isolated stock ARP client calls. Both operations are non-fatal. */
    Result StartArpWriterWorker();
    Result QueueUsbApplicationControlProperty(ncm::ProgramId application_id);
    /* Wait only for the worker item associated with this application. The
     * caller remains on the appletAE forwarding path, but this function never
     * performs ARP or USB IPC itself. */
    Result WaitForUsbApplicationControlProperty(ncm::ProgramId application_id, TimeSpan timeout);

}
