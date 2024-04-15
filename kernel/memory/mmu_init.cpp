#include "mmu_defs.hpp"

#include <limits>

#include "dtb/dtb.hpp"
#include "libk/utils.hpp"
#include "mmu_table.hpp"

#define resolve_symbol_pa(symbol)                     \
  ({                                                  \
    uintptr_t __dest;                                 \
    asm volatile("adr %x0, " #symbol : "=r"(__dest)); \
    __dest;                                           \
  })

#define resolve_symbol_va(symbol)                      \
  ({                                                   \
    uintptr_t __dest;                                  \
    asm volatile("ldr %x0, =" #symbol : "=r"(__dest)); \
    __dest;                                            \
  })

static inline constexpr void enforce(bool res) {
  if (!res) {
    libk::halt();
  }
}

static inline constexpr PagesAttributes kernel_code = {.sh = Shareability::InnerShareable,
                                                       .exec = ExecutionPermission::PrivilegedExecute,
                                                       .rw = ReadWritePermission::ReadOnly,
                                                       .access = Accessibility::Privileged,
                                                       .type = MemoryType::Normal};

static inline constexpr PagesAttributes rw_memory = {.sh = Shareability::InnerShareable,
                                                     .exec = ExecutionPermission::NeverExecute,
                                                     .rw = ReadWritePermission::ReadWrite,
                                                     .access = Accessibility::Privileged,
                                                     .type = MemoryType::Normal};

static inline constexpr PagesAttributes ro_memory = {.sh = Shareability::InnerShareable,
                                                     .exec = ExecutionPermission::NeverExecute,
                                                     .rw = ReadWritePermission::ReadOnly,
                                                     .access = Accessibility::Privileged,
                                                     .type = MemoryType::Normal};

static inline constexpr PagesAttributes device_memory = {.sh = Shareability::OuterShareable,
                                                         .exec = ExecutionPermission::NeverExecute,
                                                         .rw = ReadWritePermission::ReadWrite,
                                                         .access = Accessibility::Privileged,
                                                         .type = MemoryType::Device_nGnRnE};

static inline constexpr PagesAttributes vc_memory = {.sh = Shareability::OuterShareable,
                                                     .exec = ExecutionPermission::NeverExecute,
                                                     .rw = ReadWritePermission::ReadWrite,
                                                     .access = Accessibility::Privileged,
                                                     .type = MemoryType::Device_nGRE};

struct DeviceMemoryProperties {
  const bool is_arm_mem_address_u64;
  const bool is_arm_mem_size_u64;

  const bool is_soc_mem_address_u64;
  const bool is_soc_mem_size_u64;
};

DeviceMemoryProperties inline get_memory_properties(const DeviceTree& dt) {
  Property tmp_prop;

  enforce(dt.find_property("/#address-cells", &tmp_prop));
  const auto arm_address_cells = tmp_prop.get_u32();
  enforce(arm_address_cells.has_value() && arm_address_cells.get_value() <= 2);
  const bool is_u64_arm_mem_address = arm_address_cells.get_value() != 1;

  enforce(dt.find_property("/#size-cells", &tmp_prop));
  const auto arm_size_cells = tmp_prop.get_u32();
  enforce(arm_size_cells.has_value() && arm_size_cells.get_value() <= 2);
  const bool is_u64_arm_mem_size = arm_size_cells.get_value() != 1;

  enforce(dt.find_property("/soc/#address-cells", &tmp_prop));
  const auto soc_address_cells = tmp_prop.get_u32();
  enforce(soc_address_cells.has_value() && soc_address_cells.get_value() <= 2);
  const bool is_u64_soc_mem_address = soc_address_cells.get_value() != 1;

  enforce(dt.find_property("/soc/#size-cells", &tmp_prop));
  const auto soc_size_cells = tmp_prop.get_u32();
  enforce(soc_size_cells.has_value() && soc_size_cells.get_value() <= 2);
  const bool is_u64_soc_mem_size = soc_size_cells.get_value() != 1;

  return {is_u64_arm_mem_address, is_u64_arm_mem_size, is_u64_soc_mem_address, is_u64_soc_mem_size};
}

void inline setup_memory_mapping(MMUTable* tbl,
                                 const DeviceTree& dt,
                                 const uint32_t* dtb,
                                 const DeviceMemoryProperties& prop) {
  Property tmp_prop;

  for (const auto& node : dt.get_root().get_children()) {
    if (node.get_name().starts_with("memory@")) {
      // Found a memory node !

      enforce(node.find_property("reg", &tmp_prop));

      size_t index = 0;

      while (index < tmp_prop.length) {
        uint64_t memory_chunk_start = 0;
        uint64_t memory_chunk_size = 0;

        enforce(tmp_prop.get_variable_int(&index, &memory_chunk_start, prop.is_arm_mem_address_u64));
        enforce(tmp_prop.get_variable_int(&index, &memory_chunk_size, prop.is_arm_mem_size_u64));

        const VirtualPA va_start = NORMAL_MEMORY + memory_chunk_start;
        const VirtualPA va_end = va_start + memory_chunk_size - PAGE_SIZE;
        const PhysicalPA pa_start = memory_chunk_start;
        enforce(map_range(tbl, va_start, va_end, pa_start, rw_memory));
      }
    }
  }

  //     Mapped all normal memory available, now change memory properties for special sections
  {
    // Mapping kernel code segment in read only, no execute
    const auto va_start = VirtualPA(resolve_symbol_va(_stext));
    const auto va_end = VirtualPA(resolve_symbol_va(_srodata) - PAGE_SIZE);
    enforce(change_attr_range(tbl, va_start, va_end, kernel_code));
  }

  {
    // Mapping read-only segment of kernel
    const auto va_start = VirtualPA(resolve_symbol_va(_srodata));
    const auto va_end = VirtualPA(resolve_symbol_va(_srwdata) - PAGE_SIZE);
    enforce(change_attr_range(tbl, va_start, va_end, ro_memory));
  }

  {
    // Mapping device tree in read-only
    const auto dtb_start = PhysicalPA((uintptr_t)dtb & ~(PAGE_SIZE - 1));
    const size_t dtb_size = libk::from_be(dtb[1]);
    const auto dtb_stop = PhysicalPA(libk::align((uintptr_t)dtb + dtb_size, PAGE_SIZE));
    enforce(change_attr_range(tbl, NORMAL_MEMORY + dtb_start, NORMAL_MEMORY + dtb_stop, ro_memory));
  }
}

void inline setup_vc_mapping(MMUTable* tbl, const DeviceTree& dt, const DeviceMemoryProperties& prop) {
  uint64_t mmio_base_start = std::numeric_limits<uint64_t>::max();

  Property tmp_prop;
  enforce(dt.find_property("/soc/ranges", &tmp_prop));

  size_t index = 0;
  while (index < tmp_prop.length) {
    uint64_t memory_chunk_start = 0;

    enforce(tmp_prop.get_variable_int(&index, nullptr, prop.is_soc_mem_address_u64));
    enforce(tmp_prop.get_variable_int(&index, &memory_chunk_start, prop.is_arm_mem_address_u64));
    enforce(tmp_prop.get_variable_int(&index, nullptr, prop.is_soc_mem_size_u64));

    if (memory_chunk_start < mmio_base_start) {
      mmio_base_start = memory_chunk_start;
    }
  }

  index = 0;
  enforce(dt.find_property("/memreserve", &tmp_prop));

  uint64_t vc_start;
  enforce(tmp_prop.get_variable_int(&index, &vc_start, false));

  uint64_t vc_size;
  enforce(tmp_prop.get_variable_int(&index, &vc_size, false));

  const uint64_t final_vc_size = libk::min(vc_size, mmio_base_start - vc_start);

  enforce(map_range(tbl, VC_MEMORY, VC_MEMORY + final_vc_size - PAGE_SIZE, vc_start, vc_memory));
}

void inline setup_device_mapping(MMUTable* tbl, const DeviceTree& dt, const DeviceMemoryProperties& prop) {
  Property tmp_prop;

  enforce(dt.find_property("/soc/ranges", &tmp_prop));

  size_t index = 0;
  size_t offset = 0;
  while (index < tmp_prop.length) {
    uint64_t memory_chunk_start = 0;
    uint64_t memory_chunk_size = 0;

    enforce(tmp_prop.get_variable_int(&index, nullptr, prop.is_soc_mem_address_u64));
    enforce(tmp_prop.get_variable_int(&index, &memory_chunk_start, prop.is_arm_mem_address_u64));
    enforce(tmp_prop.get_variable_int(&index, &memory_chunk_size, prop.is_soc_mem_size_u64));

    const VirtualPA va_start = DEVICE_MEMORY + offset;
    const VirtualPA va_end = va_start + memory_chunk_size - PAGE_SIZE;
    const PhysicalPA pa_start = memory_chunk_start;
    enforce(map_range(tbl, va_start, va_end, pa_start, device_memory));
    offset += memory_chunk_size;
  }
}

void inline setup_stack_mapping(MMUTable* tbl) {
  enforce(map_range(tbl, KERNEL_STACK_PAGE_TOP((uint64_t)DEFAULT_CORE),
                    KERNEL_STACK_PAGE_BOTTOM((uint64_t)DEFAULT_CORE), 0, rw_memory));
}

void inline setup_mair() {
  static constexpr uint64_t device_nGnRnE_mair = 0b00000000;
  static constexpr uint64_t device_nGRE_mair = 0b00001000;
  static constexpr uint64_t normal_no_cache = 0b01000100;
  static constexpr uint64_t normal = 0b10111011;

  static constexpr uint64_t r = (normal << 8 * ((size_t)MemoryType::Normal)) |
                                (device_nGnRnE_mair << 8 * ((size_t)MemoryType::Device_nGnRnE)) |
                                (device_nGRE_mair << 8 * ((size_t)MemoryType::Device_nGRE)) |
                                (normal_no_cache << 8 * ((size_t)MemoryType::Normal_NoCache));
  asm volatile("msr mair_el1, %0" : : "r"(r));
}

void inline setup_tcr() {
  uint64_t r = (0b00LL << 37) |   // TBI=0, no tagging
               (0b0LL << 36) |    // AS=0, Asid if 8bit
               (0b101LL << 32) |  // IPS, address size is 48bit (256TB)

               (0b10LL << 30) |  // TG1=4k
               (0b11LL << 28) |  // SH1=3 inner
               (0b01LL << 26) |  // ORGN1=1 write back
               (0b01LL << 24) |  // IRGN1=1 write back
               (0b0LL << 23) |   // EPD1 enable higher half
               (0b0LL << 22) |   // A1=0, TTBR0 set ASID
               (16LL << 16) |    // T1SZ=16, 4 levels (256TB)

               (0b00LL << 14) |  // TG0=4k
               (0b11LL << 12) |  // SH0=3 inner
               (0b01LL << 10) |  // ORGN0=1 write back
               (0b01LL << 8) |   // IRGN0=1 write back
               (0b0LL << 7) |    // EPD0 enable lower half
               (16LL << 0);      // T0SZ=16, 4 levels (256TB)
  asm volatile("msr tcr_el1, %0" : : "r"(r));
  asm volatile("isb");
}

void inline setup_ttbr0_ttbr1(MMUTable* tbl) {
  static constexpr uint64_t TTBR_CNP = 0x1;

  // lower half, user space
  asm volatile("msr ttbr0_el1, %0" : : "r"(tbl->pgd + TTBR_CNP));
  // upper half, kernel space
  asm volatile("msr ttbr1_el1, %0" : : "r"(tbl->pgd + TTBR_CNP));

  asm volatile("dsb ish; isb;");
}

void inline setup_sctlr() {
  uint64_t r;
  asm volatile("mrs %0, sctlr_el1" : "=r"(r));
  r |= 0xC00800;      // set mandatory reserved bits
  r &= ~((1 << 26) |  // clear UCI, no access cache maintenance in EL0

         (1 << 25) |  // clear EE, little endian translation tables
         (1 << 24) |  // clear E0E, data access are little endian

         (1 << 19) |  // clear WXN, writable do not imply execute never

         (1 << 18) |  // clear nTWE, no access to wfe instruction in EL0
         (1 << 16) |  // clear nTWI, no access to wfi instruction in EL0

         (1 << 15) |  // clear UCT, no access cache control in EL0
         (1 << 14) |  // clear DZE, no access cache reset in EL0
         (1 << 9) |   // clear UMA, no access to system registers in EL0

         (1 << 4) |  // clear SA0, no stack alignment check in EL0
         (1 << 3) |  // clear SA, no stack alignment check in EL1
         (1 << 1));  // clear A, no alignment check

  r |= (1 << 0) |  // set M, enable MMU
       (1 << 2) |  // set C, enable caching of normal memory
       (1 << 12);  // set I, enable instruction cache

  asm volatile("msr sctlr_el1, %0" : : "r"(r));
  asm volatile("isb");
}

struct MMUTableHandleData {
  const size_t first_page;
  const size_t upper_bound;
  size_t nb_allocated;
};

extern "C" VirtualPA alloc_page(void* handle_ptr) {
  auto* handle = (MMUTableHandleData*)handle_ptr;

  uintptr_t current_page_offset = handle->first_page + PAGE_SIZE * handle->nb_allocated;
  enforce(current_page_offset + PAGE_SIZE < handle->upper_bound);
  uint64_t* new_page = (uint64_t*)current_page_offset;
  handle->nb_allocated++;

  for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); ++i) {
    new_page[i] = 0;
  }

  return (VirtualPA)new_page;
}

extern "C" VirtualPA resolve_pa(void*, PhysicalPA pa) {
  return pa;
}

extern "C" PhysicalPA resolve_va(void*, VirtualPA va) {
  return va;
}

extern "C" void mmu_init(const uint32_t* dtb) {
  const auto alloc_start = PhysicalPA(resolve_symbol_pa(_kend));
  const auto dtb_start = PhysicalPA((uintptr_t)dtb & ~(PAGE_SIZE - 1));

  MMUTableHandleData handle = {
      .first_page = alloc_start,
      .upper_bound = dtb_start,
      .nb_allocated = 0,
  };

  VirtualPA pgd = alloc_page(&handle);

  MMUTable tbl = {
      .kind = MMUTable::Kind::Kernel,
      .pgd = pgd,
      .asid = 0,
      .handle = (void*)&handle,
      .alloc = (AllocFun)resolve_symbol_pa(alloc_page),
      .free = nullptr,  // We don't free anything here !
      .resolve_pa = (ResolvePA)resolve_symbol_pa(resolve_pa),
      .resolve_va = (ResolveVA)resolve_symbol_pa(resolve_va),
  };

  const DeviceTree dt(dtb);
  enforce(dt.is_status_okay());

  const auto mem_prop = get_memory_properties(dt);
  setup_memory_mapping(&tbl, dt, dtb, mem_prop);
  setup_vc_mapping(&tbl, dt, mem_prop);
  setup_device_mapping(&tbl, dt, mem_prop);
  setup_stack_mapping(&tbl);

  libk::write64(resolve_symbol_pa(_mmu_init_data) + 0 * sizeof(uint64_t), pgd);
  libk::write64(resolve_symbol_pa(_mmu_init_data) + 1 * sizeof(uint64_t), handle.first_page);
  libk::write64(resolve_symbol_pa(_mmu_init_data) + 2 * sizeof(uint64_t), handle.nb_allocated);

  setup_mair();
  setup_tcr();
  setup_ttbr0_ttbr1(&tbl);
  setup_sctlr();
}