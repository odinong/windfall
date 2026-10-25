#pragma once

#define FSC_STATUS_UNDEFINED (-1)
#define FSC_STATUS_OK (0)
#define FSC_STATUS_FILE_NOT_FOUND (1)

enum class FSC_ACCESS_FLAG : uint8
{
    NONE = 0,
    READ_PERMISSION = (1 << 0),
    WRITE_PERMISSION = (1 << 1),
    OPEN_FILE = (1 << 2),
    OPEN_DIR = (1 << 3),
};
ENABLE_BITMASK_OPERATORS(FSC_ACCESS_FLAG);

struct FSCVirtualFile;
FSCVirtualFile* fsc_open(const char* path, FSC_ACCESS_FLAG accessFlags, sint32* fscStatus, sint32 maxPriority = 0);
void fsc_close(FSCVirtualFile* file);
uint32 fsc_getFileSize(FSCVirtualFile* file);
uint32 fsc_readFile(FSCVirtualFile* file, void* buffer, uint32 size);
uint32 fsc_setFileSeek(FSCVirtualFile* file, uint64 seek);
uint64 fsc_getFileSeek(FSCVirtualFile* file);
bool fsc_doesFileExist(const char* path, sint32 maxPriority = 0);
