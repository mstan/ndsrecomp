// Runner-owned Platform additions consumed only by the optional verbatim
// melonDS OpenGLSupport.cpp translation unit. GPL-3.0-or-later; the interface
// signatures are derived from melonDS Platform.h.
#pragma once

#include <string>

#include "Platform.h"

namespace melonDS::Platform
{
enum FileMode : unsigned
{
    None = 0,
    Read = 0x01,
    Write = 0x02,
    Preserve = 0x04,
    NoCreate = 0x08,
    Text = 0x10,
    Append = 0x20,
    ReadWrite = Read | Write,
};

enum class FileSeekOrigin
{
    Start,
    Current,
    End,
};

struct FileHandle;
FileHandle* OpenLocalFile(const std::string& path, FileMode mode);
bool CloseFile(FileHandle* file);
bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin);
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file);
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file);
}
