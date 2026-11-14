#pragma once
#include <vapours.hpp>
namespace ams::fs {
struct FileHandle { void *handle; };
struct WriteOption { unsigned value; static const WriteOption None, Flush; };
inline const WriteOption WriteOption::None{0}, WriteOption::Flush{1};
Result ReadFile(size_t *, FileHandle, s64, void *, size_t);
Result ReadFile(FileHandle, s64, void *, size_t);
Result GetFileSize(s64 *, FileHandle);
Result WriteFile(FileHandle, s64, const void *, size_t, const WriteOption &);
Result FlushFile(FileHandle);
void CloseFile(FileHandle);
}
