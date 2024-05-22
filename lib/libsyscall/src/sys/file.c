#include <assert.h>
#include <sys/file.h>
#include <sys/syscall.h>

sys_file_t* sys_open_file(const char* path, sys_file_mode_t mode) {
  return (sys_file_t*)__syscall2(SYS_OPEN_FILE, (sys_word_t)path, (sys_word_t)mode);
}

void sys_close_file(sys_file_t* file) {
  assert(file != NULL);
  __syscall1(SYS_CLOSE_FILE, (sys_word_t)file);
}

sys_error_t sys_file_read(sys_file_t* file, void* buffer, size_t bytes_to_read, size_t* read_bytes) {
  assert(file != NULL);
  assert(bytes_to_read == 0 || buffer != NULL);

  return __syscall4(SYS_READ_FILE, (sys_word_t)file, (sys_word_t)buffer, bytes_to_read, (sys_word_t)read_bytes);
}

size_t sys_get_file_size(sys_file_t* file) {
  assert(file != NULL);
  return __syscall1(SYS_GET_FILE_SIZE, (sys_word_t)file);
}
