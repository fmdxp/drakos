#pragma once

bool run_address_space_runtime_test();
void address_space_runtime_expect_fault(uintptr_t fault_address, uint64_t vector);
bool address_space_runtime_record_fault(uint64_t vector, uintptr_t fault_address);
bool address_space_runtime_fault_seen();
bool address_space_runtime_fault_matches();
bool address_space_runtime_fault_mapping(uint64_t* flags, bool* owned);
