#include "filesystem.hpp"
#include <sys/file.h>
#include <libk/log.hpp>
#include <libk/string.hpp>

#include "fat/ff.h"

FileSystem& FileSystem::get() {
  static FileSystem instance;
  return instance;
}

static FATFS fatfs;

void FileSystem::init() {
  auto error_code = f_mount(&fatfs, "/", 1);
  if (error_code != FR_OK) {
    LOG_CRITICAL("Failed to initialize the FAT filesystem (code = {})", error_code);
    return;
  }
}

File* FileSystem::open(const char* path, int flags) {
  File* file = new File;

  BYTE mode = 0;
  if ((flags & SYS_FM_READ) != 0)
    mode |= FA_READ;
  if ((flags & SYS_FM_WRITE) != 0)
    mode |= FA_WRITE;

  if (f_open(&file->m_handle, path, mode) == FR_OK)
    return file;

  delete file;
  return nullptr;
}

void FileSystem::close(File* handle) {
  KASSERT(handle != nullptr);

  f_close(&handle->m_handle);
  delete handle;
}
