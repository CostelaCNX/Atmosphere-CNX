/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstdint>
namespace nexus_startup {
/* Result is zero on success. The backend closes the file before WriteFlushed
 * returns. Only this attempt's exclusively-created temporary file is removed;
 * a final report is never deleted, including after an uncertain commit. */
template<class Backend>
uint32_t PublishReport(Backend &backend,const char *temporary,const char *final,
                       const char *data,size_t size) {
    uint32_t result=backend.CreateExclusive(temporary,size);
    if(result)return result;
    result=backend.WriteFlushed(temporary,data,size);
    if(!result)result=backend.Rename(temporary,final);
    if(result){backend.DeleteTemporary(temporary);return result;}
    return backend.Commit();
}
}
