#pragma once
#include <stratosphere/fs/fs_file.hpp>
namespace ams::fs {
enum { OpenMode_Read = 1, OpenMode_Write = 2 };
Result OpenFile(FileHandle *, const char *, int);
Result DeleteFile(const char *);
Result CreateFile(const char *, s64);
Result RenameFile(const char *, const char *);
}
