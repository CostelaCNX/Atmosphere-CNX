#pragma once
#include <vapours.hpp>
namespace ams::fs {
constexpr size_t EntryNameLengthMax = 0x301;
enum DirectoryEntryType { DirectoryEntryType_Directory, DirectoryEntryType_File };
using ResultPathNotFound = Error<0x202>;
Result GetEntryType(DirectoryEntryType *, const char *);
}
