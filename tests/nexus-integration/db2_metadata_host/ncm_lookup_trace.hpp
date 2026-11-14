#pragma once
#include <stratosphere.hpp>
namespace ams::ncm::usb_recovery{enum class LookupOperation{MetadataContentInfo,MetadataLatestKey};}
namespace ams::ncm::lookup_trace{inline void Metadata(usb_recovery::LookupOperation,StorageId,u64,u32,ContentType,util::optional<u8>,Result,const ContentId* = nullptr){}}
