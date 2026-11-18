#pragma once

bool run_address_space_runtime_test();
bool run_smp_user_scheduler_runtime_test();
bool run_smp_kernel_scheduler_runtime_test();
bool run_smp_independent_user_runtime_test();
void address_space_runtime_expect_fault(uintptr_t fault_address, uint64_t vector);
struct Context;
class Process;
bool address_space_runtime_record_fault(uint64_t vector, uintptr_t fault_address,
                                        const Context* context);
void address_space_runtime_expect_user_launch(Process* process, uintptr_t fault_ip,
                                              uintptr_t stack_pointer,
                                              uintptr_t marker_address);
bool address_space_runtime_user_launch_seen();
bool address_space_runtime_user_launch_matched();
bool address_space_runtime_fault_seen();
bool address_space_runtime_fault_matches();
bool address_space_runtime_fault_mapping(uint64_t* flags, bool* owned);
