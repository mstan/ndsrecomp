// GPL-3.0-or-later; see PlatformCompute.h and THIRD_PARTY_ATTRIBUTION.md.
#include "PlatformCompute.h"

#include <cstdio>

namespace melonDS::Platform
{
struct FileHandle
{
    std::FILE* file = nullptr;
};

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    const bool read = (mode & Read) != 0;
    const bool write = (mode & Write) != 0;
    const bool preserve = (mode & Preserve) != 0;
    const bool append = (mode & Append) != 0;
    const bool text = (mode & Text) != 0;
    const char* openMode = nullptr;
    if (append) openMode = read ? (text ? "a+" : "a+b")
                                : (text ? "a" : "ab");
    else if (read && write) openMode = preserve ? (text ? "r+" : "r+b")
                                                : (text ? "w+" : "w+b");
    else if (write) openMode = text ? "w" : "wb";
    else openMode = text ? "r" : "rb";

    std::FILE* file = std::fopen(path.c_str(), openMode);
    if (!file && write && preserve && !(mode & NoCreate))
        file = std::fopen(path.c_str(), text ? "w+" : "w+b");
    return file ? new FileHandle{file} : nullptr;
}

bool CloseFile(FileHandle* file)
{
    if (!file) return false;
    const bool ok = std::fclose(file->file) == 0;
    delete file;
    return ok;
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    if (!file) return false;
    const int whence = origin == FileSeekOrigin::Start ? SEEK_SET
                     : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
#if defined(_WIN32)
    return _fseeki64(file->file, offset, whence) == 0;
#else
    return std::fseek(file->file, static_cast<long>(offset), whence) == 0;
#endif
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return file ? std::fread(data, static_cast<size_t>(size),
                             static_cast<size_t>(count), file->file) : 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return file ? std::fwrite(data, static_cast<size_t>(size),
                              static_cast<size_t>(count), file->file) : 0;
}
}
