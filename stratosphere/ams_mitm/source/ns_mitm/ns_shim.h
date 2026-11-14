/**
 * @file ns_shim.h
 * @brief Nintendo Shell Services (ns) IPC wrapper.
 * @author SciresM
 * @copyright libnx Authors
 */
#pragma once
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    Service s;
} NsDocumentInterface;

typedef struct {
    Service s;
} NsApplicationManagerInterface;

typedef struct {
    Service s;
} NsReadOnlyApplicationControlDataInterface;

/* Command forwarders. */
Result nsGetDocumentInterfaceFwd(Service* s, NsDocumentInterface* out);
Result nsGetApplicationManagerInterfaceFwd(Service* s, NsApplicationManagerInterface* out);
Result nsGetReadOnlyApplicationControlDataInterfaceFwd(Service* s, NsReadOnlyApplicationControlDataInterface* out);

Result nsamGetApplicationContentPathFwd(Service* s, void* out, size_t out_size, u64 app_id, NcmContentType content_type);
Result nsamResolveApplicationContentPathFwd(Service* s, u64 app_id, NcmContentType content_type);
Result nsamGetRunningApplicationProgramIdFwd(Service* s, u64* out_program_id, u64 app_id);
Result nsamGetApplicationViewFwd(Service* s, u32 command_id, void* out_views, size_t out_views_size, const void* application_ids, size_t application_ids_size);
Result nsamCheckApplicationLaunchVersionFwd(Service* s, u64 app_id);

Result nswebGetApplicationContentPath(NsDocumentInterface* doc, void* out, size_t out_size, u8 *out_attr, u64 app_id, NcmContentType content_type);
Result nswebResolveApplicationContentPath(NsDocumentInterface* doc, u64 app_id, NcmContentType content_type);
Result nswebGetRunningApplicationProgramId(NsDocumentInterface* doc, u64* out_program_id, u64 app_id);
Result nswebGetApplicationContentPath2(NsDocumentInterface* doc, void* out, size_t out_size, u64* out_program_id, u8 *out_attr, u64 app_id, NcmContentType content_type);

void nsDocumentInterfaceClose(NsDocumentInterface* doc);
void nsApplicationManagerInterfaceClose(NsApplicationManagerInterface* am);
void nsReadOnlyApplicationControlDataInterfaceClose(NsReadOnlyApplicationControlDataInterface* control);

#ifdef __cplusplus
}
#endif
