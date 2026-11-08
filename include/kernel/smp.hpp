#pragma once

#include <stdint.h>

bool smp_initialize();
uint32_t smp_online_cpu_count();
bool smp_is_bootstrap_processor();
bool smp_tlb_invalidate_page(uintptr_t virtual_address);
bool smp_tlb_flush_non_global();
bool smp_address_space_active(uintptr_t root_phys);
bool smp_test_switch_address_space(uintptr_t root_phys);
bool smp_test_read_virtual(uintptr_t virtual_address, uint64_t expected_value);
bool smp_test_timeout_recovery();
void smp_handle_ipi();
