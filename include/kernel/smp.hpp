#pragma once

#include <stdint.h>

bool smp_initialize();
uint32_t smp_online_cpu_count();
uint64_t smp_ap_syscall_count();
void smp_note_syscall();
bool smp_is_bootstrap_processor();
uint32_t smp_current_processor_index();
bool smp_processor_online(uint32_t processor_index);
bool smp_start_application_processors();
void smp_set_kernel_stack(uintptr_t stack);
void smp_set_local_active_root(uintptr_t root_phys);
bool smp_tlb_invalidate_page(uintptr_t virtual_address);
bool smp_tlb_flush_non_global();
bool smp_address_space_active(uintptr_t root_phys);
bool smp_test_switch_address_space(uintptr_t root_phys);
bool smp_test_read_virtual(uintptr_t virtual_address, uint64_t expected_value);
bool smp_test_timeout_recovery();
void smp_handle_ipi();
