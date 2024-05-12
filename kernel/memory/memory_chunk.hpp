#pragma once
#include "memory/mmu_table.hpp"

#include <libk/linked_list.hpp>

class ProcessMemory;

class MemoryChunk {
 public:
  /** Creates a memory chunk of @a nb_pages continuous pages. */
  MemoryChunk(size_t nb_pages);

  /** Free this memory chunk. */
  ~MemoryChunk();

  /** Checks if the memory has been allocated. YOU MUST DO IT. */
  [[nodiscard]] bool is_status_okay() const;

  /** Write @a data_byte_length bytes of @a data at offset @a byte_offset.
   * @returns the number of bytes written.
   * Can be different of @a data_byte_length when @a byte_offset is too big. */
  [[nodiscard]] size_t write(size_t byte_offset, const void* data, size_t data_byte_length);

  /** Read @a data_byte_length bytes into @a data from offset @a byte_offset.
   * @returns the number of bytes read.
   * Can be different of @a data_byte_length when @a byte_offset is too big. */
  [[nodiscard]] size_t read(size_t byte_offset, void* data, size_t data_byte_length) const;

  /** Free this memory chunk and all it's mapping in all process memory. */
  void free();

  /** Returns the number of bytes of this chunk. */
  [[nodiscard]] size_t byte_size() const;

  /** @returns the size of a page. */
  [[gnu::always_inline, clang::always_inline]] static inline size_t page_byte_size();

 private:
  friend ProcessMemory;

  const size_t _nb_pages;
  PhysicalPA* _pas;

  VirtualPA _kernel_va;

  struct ProcessMapped {
    VirtualPA chunk_start;
    ProcessMemory* mem;
  };

  libk::LinkedList<ProcessMapped> _proc;

  void register_mapping(ProcessMemory* proc_mem, VirtualPA start_addr);
  void unregister_mapping(ProcessMemory* proc_mem);
  VirtualPA end_address(VirtualPA start_address);
};
