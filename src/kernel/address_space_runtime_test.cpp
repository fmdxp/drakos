#include "kernel/address_space_runtime_test.hpp"
#include "kernel/elf.hpp"
#include "kernel/user_process.hpp"
#include "kernel/panic.hpp"
#include "fs/vfs.hpp"

#include "kernel/boot_diagnostics.hpp"
#include "kernel/thread.hpp"
#include "kernel/smp.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"
#include "cpu.hpp"

extern "C" const uint8_t user_args_test_start[];
extern "C" const uint8_t user_args_test_success_fault[];
extern "C" const uint8_t user_args_test_end[];

static volatile bool s_expect_user_fault = false;
static volatile bool s_user_fault_seen = false;
static volatile bool s_user_fault_matched = false;
static volatile bool s_user_fault_mapping_found = false;
static volatile uint64_t s_user_fault_mapping_flags = 0;
static volatile bool s_user_fault_mapping_owned = false;
static volatile bool s_expect_user_launch = false;
static volatile bool s_user_launch_seen = false;
static volatile bool s_user_launch_matched = false;
static volatile bool s_user_launch_vector_ok = false;
static volatile bool s_user_launch_cpl_ok = false;
static volatile bool s_user_launch_ip_ok = false;
static volatile bool s_user_launch_offset_ok = false;
static volatile bool s_user_launch_stack_ok = false;
static volatile bool s_user_launch_owner_ok = false;
static volatile bool s_user_launch_marker_ok = false;
static volatile bool s_user_launch_arguments_ok = false;
static volatile uint64_t s_user_launch_actual_vector = 0;
static volatile uintptr_t s_user_launch_actual_ip = 0;
static uintptr_t s_expected_fault_address = 0;
static uint64_t s_expected_fault_vector = 0;
static Process* s_expected_launch_process = nullptr;
static uintptr_t s_expected_launch_fault_ip = 0;
static uintptr_t s_expected_launch_stack = 0;
static uintptr_t s_expected_launch_marker = 0;
static uint64_t s_expected_launch_pid = 0;
static uint64_t s_smp_kernel_counter = 0;

static void diagnostic_write_u64(const char* prefix, uint64_t value) {
    char digits[21];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    boot_diagnostics_write(prefix);
    while (count) {
        char character[2] = {digits[--count], 0};
        boot_diagnostics_write(character);
    }
    boot_diagnostics_write("\n");
}

class RuntimeElfFileSystem final : public VFS::FileSystem {
public:
    uint8_t image[0x3000]{};

    VFS::Node* lookup(const char*) override { return nullptr; }
    VFS::Node* create(const char*, bool) override { return nullptr; }
    int read(VFS::Node*, void* output, uint32_t offset, uint32_t size) override {
        if (!output || offset >= sizeof(image)) return 0;
        if (size > sizeof(image) - offset) size = sizeof(image) - offset;
        auto* bytes = static_cast<uint8_t*>(output);
        for (uint32_t i = 0; i < size; ++i) bytes[i] = image[offset + i];
        return static_cast<int>(size);
    }
    int write(VFS::Node*, const void*, uint32_t, uint32_t) override { return -1; }
    bool listdir(VFS::Node*, VFS::DirEntry*, uint32_t, uint32_t& count) override {
        count = 0;
        return true;
    }
    void release(VFS::Node* node) override { delete node; }

    void reset_image() {
        for (size_t i = 0; i < sizeof(image); ++i) image[i] = 0;
        auto* header = reinterpret_cast<Elf64_Ehdr*>(image);
        header->e_ident[0] = 0x7F;
        header->e_ident[1] = 'E';
        header->e_ident[2] = 'L';
        header->e_ident[3] = 'F';
        header->e_ident[EI_CLASS] = ELFCLASS64;
        header->e_ident[EI_DATA] = ELFDATA2LSB;
        header->e_ident[EI_VERSION] = EV_CURRENT;
        header->e_type = ET_EXEC;
        header->e_machine = EM_X86_64;
        header->e_version = EV_CURRENT;
        header->e_entry = 0x00400000;
        header->e_phoff = sizeof(Elf64_Ehdr);
        header->e_ehsize = sizeof(Elf64_Ehdr);
        header->e_phentsize = sizeof(Elf64_Phdr);
        header->e_phnum = 2;
        auto* programs = reinterpret_cast<Elf64_Phdr*>(image + sizeof(Elf64_Ehdr));
        size_t test_code_size = static_cast<size_t>(user_args_test_end - user_args_test_start);
        programs[0] = {PT_LOAD, PF_R | PF_X, 0x1000, 0x00400000, 0,
                       test_code_size, PAGE_SIZE, PAGE_SIZE};
        programs[1] = {PT_LOAD, PF_R | PF_W, 0x2000, 0x00500000, 0, 8, PAGE_SIZE, PAGE_SIZE};
        for (size_t i = 0; i < test_code_size; ++i)
            image[0x1000 + i] = user_args_test_start[i];
    }
};

static void smp_kernel_counter_thread() {
    for (;;) __atomic_add_fetch(&s_smp_kernel_counter, 1, __ATOMIC_RELAXED);
}

static void install_smp_user_counter_program(uintptr_t frame) {
    constexpr uintptr_t data_va = 0x00401000;
    const uint8_t program[] = {
        0x48, 0xBB, 0x00, 0x10, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xB8, 0x27, 0x00, 0x00, 0x00, 0x0F, 0x05,
        0xF0, 0x48, 0xFF, 0x03, 0xEB, 0xF3
    };
    auto* code = reinterpret_cast<volatile uint8_t*>(frame + pmm_hhdm_offset());
    for (size_t i = 0; i < sizeof(program); ++i) code[i] = program[i];
}

static bool wait_address_space_destroyed(uintptr_t root) {
    for (size_t attempt = 0; attempt < 128; ++attempt) {
        VmmAddressSpaceResult result = vmm_retain_address_space(root);
        if (result == VmmAddressSpaceResult::InvalidAddressSpace) return true;
        if (result == VmmAddressSpaceResult::Success)
            (void)vmm_release_address_space(root);
        scheduler_yield();
    }
    return false;
}

void address_space_runtime_expect_fault(uintptr_t fault_address, uint64_t vector) {
    s_expected_fault_address = fault_address;
    s_expected_fault_vector = vector;
    s_user_fault_seen = false;
    s_user_fault_matched = false;
    s_user_fault_mapping_found = false;
    s_user_fault_mapping_flags = 0;
    s_user_fault_mapping_owned = false;
    s_expect_user_fault = true;
}

void address_space_runtime_expect_user_launch(Process* process, uintptr_t fault_ip,
                                              uintptr_t stack_pointer,
                                              uintptr_t marker_address) {
    s_expected_launch_process = process;
    s_expected_launch_fault_ip = fault_ip;
    s_expected_launch_stack = stack_pointer;
    s_expected_launch_marker = marker_address;
    s_expected_launch_pid = process ? process->pid : 0;
    s_user_launch_seen = false;
    s_user_launch_matched = false;
    s_user_launch_vector_ok = false;
    s_user_launch_cpl_ok = false;
    s_user_launch_ip_ok = false;
    s_user_launch_offset_ok = false;
    s_user_launch_stack_ok = false;
    s_user_launch_owner_ok = false;
    s_user_launch_marker_ok = false;
    s_user_launch_arguments_ok = false;
    __atomic_store_n(&s_expect_user_launch, true, __ATOMIC_RELEASE);
}

bool address_space_runtime_user_launch_seen() { return s_user_launch_seen; }
bool address_space_runtime_user_launch_matched() { return s_user_launch_matched; }

bool address_space_runtime_record_fault(uint64_t vector, uintptr_t fault_address,
                                        const Context* context) {
    if (__atomic_load_n(&s_expect_user_launch, __ATOMIC_ACQUIRE)) {
        s_user_launch_seen = true;
        s_user_launch_actual_vector = vector;
        s_user_launch_actual_ip = context ? context->rip : 0;
        Thread* current = scheduler_get_current_thread();
        s_user_launch_vector_ok = vector == 6;
        s_user_launch_cpl_ok = context && (context->cs & 3) == 3;
        s_user_launch_ip_ok = context && context->rip == s_expected_launch_fault_ip;
        s_user_launch_offset_ok = context && context->rip == s_expected_launch_fault_ip;
        s_user_launch_stack_ok = context && context->rsp == s_expected_launch_stack;
        s_user_launch_owner_ok = current &&
            current->parent_process == s_expected_launch_process &&
            current->parent_process->is_user &&
            current->parent_process->page_table_phys != vmm_get_kernel_pml4();
        VmmMappingInfo marker_mapping{};
        bool marker_mapped = vmm_query_mapping(s_expected_launch_marker, &marker_mapping) &&
            (marker_mapping.flags & (VMM_PRESENT | VMM_USER | VMM_WRITE)) ==
                (VMM_PRESENT | VMM_USER | VMM_WRITE);
        s_user_launch_marker_ok = marker_mapped;
        if (marker_mapped) {
            auto* saved_stack = reinterpret_cast<volatile uint64_t*>(
                marker_mapping.physical_page + pmm_hhdm_offset());
            auto* saved_pid = reinterpret_cast<volatile uint64_t*>(
                reinterpret_cast<volatile uint8_t*>(saved_stack) + sizeof(uint64_t));
            s_user_launch_marker_ok &= *saved_stack == s_expected_launch_stack &&
                                       *saved_pid == s_expected_launch_pid;
        }
        VmmMappingInfo stack_mapping{};
        s_user_launch_arguments_ok = context &&
            vmm_query_mapping(context->rsp, &stack_mapping) &&
            (stack_mapping.flags & (VMM_PRESENT | VMM_USER)) == (VMM_PRESENT | VMM_USER);
        if (s_user_launch_arguments_ok) {
            auto* stack_page = reinterpret_cast<const uint8_t*>(
                stack_mapping.physical_page + pmm_hhdm_offset());
            auto* words = reinterpret_cast<const uint64_t*>(
                stack_page + (context->rsp & (PAGE_SIZE - 1)));
            s_user_launch_arguments_ok = words[0] == 2 && words[1] && words[2] &&
                words[3] == 0 && words[4] && words[5] == 0 &&
                words[6] == 0 && words[7] == 0;
            if (s_user_launch_arguments_ok) {
                auto* argv0 = reinterpret_cast<const char*>(stack_page +
                    (words[1] & (PAGE_SIZE - 1)));
                auto* argv1 = reinterpret_cast<const char*>(stack_page +
                    (words[2] & (PAGE_SIZE - 1)));
                auto* env0 = reinterpret_cast<const char*>(stack_page +
                    (words[4] & (PAGE_SIZE - 1)));
                s_user_launch_arguments_ok = argv0[0] == 'a' && argv0[1] == 'l' &&
                    argv0[2] == 'p' && argv0[3] == 'h' && argv0[4] == 'a' &&
                    argv0[5] == 0 && argv1[0] == 'b' && argv1[1] == 'e' &&
                    argv1[2] == 't' && argv1[3] == 'a' && argv1[4] == 0 &&
                    env0[0] == 'M' &&
                    env0[1] == 'O' && env0[2] == 'D' && env0[3] == 'E' &&
                    env0[4] == '=' && env0[5] == 't' && env0[6] == 'e' &&
                    env0[7] == 's' && env0[8] == 't' && env0[9] == 0;
            }
        }
        s_user_launch_matched = s_user_launch_vector_ok && s_user_launch_cpl_ok &&
            s_user_launch_ip_ok && s_user_launch_offset_ok &&
            s_user_launch_stack_ok && s_user_launch_owner_ok && s_user_launch_marker_ok &&
            s_user_launch_arguments_ok;
        __atomic_store_n(&s_expect_user_launch, false, __ATOMIC_RELEASE);
        return true;
    }
    if (!s_expect_user_fault) return false;
    s_user_fault_seen = true;
    s_user_fault_matched = vector == s_expected_fault_vector &&
        (vector != 14 || fault_address == s_expected_fault_address);
    if (vector == 14) {
        VmmMappingInfo mapping = {};
        s_user_fault_mapping_found = vmm_query_mapping(fault_address, &mapping);
        if (s_user_fault_mapping_found) {
            s_user_fault_mapping_flags = mapping.flags;
            s_user_fault_mapping_owned = mapping.ownership == VmmMappingInfo::Ownership::Owned;
        }
    }
    s_expect_user_fault = false;
    return true;
}

bool address_space_runtime_fault_seen() { return s_user_fault_seen; }
bool address_space_runtime_fault_matches() { return s_user_fault_matched; }
bool address_space_runtime_fault_mapping(uint64_t* flags, bool* owned) {
    if (flags) *flags = s_user_fault_mapping_flags;
    if (owned) *owned = s_user_fault_mapping_owned;
    return s_user_fault_mapping_found;
}

static void runtime_test_thread_entry() {
    for (;;) asm volatile("hlt");
}

static Process* create_user_test_process() {
    Process* process = nullptr;
    return Process::create_user(&process) == ProcessResult::Success ? process : nullptr;
}

static bool release_test_process(Process* process) {
    return !process || process->release() == ProcessResult::Success;
}

static bool run_empty_process_creation_test() {
    Process* process = create_user_test_process();
    if (!process) return false;
    uintptr_t root = process->page_table_phys;
    bool initialized_without_thread = process->state() == ProcessState::Ready &&
        process->reference_count() == 1 && process->scheduler_threads == 0;
    bool released = process->release() == ProcessResult::Success;
    bool address_space_reclaimed =
        vmm_retain_address_space(root) == VmmAddressSpaceResult::InvalidAddressSpace;
    bool passed = initialized_without_thread && released && address_space_reclaimed;
    boot_diagnostics_write(passed ? "process empty-create teardown: PASS\n" :
                                    "process empty-create teardown: FAIL\n");
    return passed;
}

static UserProcessCreateResult attempt_initial_user_process(RuntimeElfFileSystem& fs,
                                                             int failure_stage,
                                                             UserProcessImage* output) {
    auto* node = new VFS::Node{};
    if (!node) return {};
    node->type = VFS::NodeType::FILE;
    node->size = sizeof(fs.image);
    node->fs = &fs;
    VFS::FileObject file(node, VFS_O_READ);
    switch (failure_stage) {
        case 1: user_process_test_fail_next_thread_allocation(); break;
        case 2: thread_test_fail_next_user_stack_allocation(); break;
        case 3: thread_test_fail_next_user_stack_mapping(); break;
        case 4: thread_test_fail_next_initialization(); break;
        case 5: scheduler_test_fail_next_publication(); break;
        default: break;
    }
    static const char* const argv[] = {"alpha", "beta"};
    static const char* const envp[] = {"MODE=test"};
    const UserProcessArguments arguments{argv, 2, envp, 1};
    return user_process_create_from_elf(file, 0, sizeof(fs.image), output, &arguments);
}

static bool run_initial_user_thread_test() {
    bool ok = true;
    RuntimeElfFileSystem fs;
    fs.reset_image();
    const UserProcessCreateError expected_errors[] = {
        UserProcessCreateError::ElfLoadFailed,
        UserProcessCreateError::ThreadAllocationFailed,
        UserProcessCreateError::ThreadInitializationFailed,
        UserProcessCreateError::ThreadInitializationFailed,
        UserProcessCreateError::ThreadInitializationFailed,
        UserProcessCreateError::SchedulerPublicationFailed,
    };
    for (int failure_stage = 0; failure_stage <= 5; ++failure_stage) {
        fs.reset_image();
        if (failure_stage == 0)
            reinterpret_cast<Elf64_Ehdr*>(fs.image)->e_ident[0] = 0;
        UserProcessImage output{};
        UserProcessCreateResult failed = attempt_initial_user_process(fs, failure_stage, &output);
        bool correct = failed.error == expected_errors[failure_stage] && !output.process;
        if (failure_stage == 2)
            correct &= failed.thread_error == ThreadInitResult::UserStackOutOfMemory;
        if (failure_stage == 3)
            correct &= failed.thread_error == ThreadInitResult::UserStackMappingFailed &&
                       failed.thread_mapping_error == VmmMapResult::PageTableOutOfMemory;
        if (failure_stage == 4)
            correct &= failed.thread_error == ThreadInitResult::InjectedFailure;
        if (failure_stage == 5)
            correct &= failed.scheduler_error == SchedulerPublishResult::InjectedFailure;
        ok &= correct;
        boot_diagnostics_write(correct ? "user launch rollback stage: PASS\n" :
                                          "user launch rollback stage: FAIL\n");
    }

    fs.reset_image();
    UserProcessImage output{};
    uint64_t saved_flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");
    UserProcessCreateResult created = attempt_initial_user_process(fs, -1, &output);
    bool setup_ok = created.error == UserProcessCreateError::Success && output.process &&
        output.entry_point == 0x00400000 && output.initial_stack_pointer &&
        !(output.initial_stack_pointer & 0xF) &&
        output.process->state() == ProcessState::Ready &&
        output.mapped_memory_size == 2 * PAGE_SIZE && output.load_segment_count == 2 &&
        output.process->scheduler_threads == 1;
    if (setup_ok) {
        address_space_runtime_expect_user_launch(output.process,
            output.entry_point + (user_args_test_success_fault - user_args_test_start),
            output.initial_stack_pointer, 0x00500000);
    }
    asm volatile("pushq %0; popfq" :: "r"(saved_flags) : "memory", "cc");

    if (!setup_ok) {
        if (output.process && output.process->release() != ProcessResult::Success)
            panic("Unable to release failed initial user-thread test process");
        boot_diagnostics_write("initial user thread: FAIL (preparation)\n");
        return false;
    }

    for (size_t attempt = 0; attempt < 1000 && !address_space_runtime_user_launch_seen(); ++attempt)
        scheduler_yield();
    bool entered = address_space_runtime_user_launch_seen() &&
                   address_space_runtime_user_launch_matched();
    boot_diagnostics_write(s_user_launch_vector_ok ? "user launch fault vector: PASS\n" : "user launch fault vector: FAIL\n");
    boot_diagnostics_write(s_user_launch_cpl_ok ? "user launch privilege: PASS\n" : "user launch privilege: FAIL\n");
    boot_diagnostics_write(s_user_launch_ip_ok ? "user launch entry IP: PASS\n" : "user launch entry IP: FAIL\n");
    boot_diagnostics_write(s_user_launch_offset_ok ? "user launch entry offset: PASS\n" : "user launch entry offset: FAIL\n");
    boot_diagnostics_write(s_user_launch_stack_ok ? "user launch stack pointer: PASS\n" : "user launch stack pointer: FAIL\n");
    boot_diagnostics_write(s_user_launch_owner_ok ? "user launch process association: PASS\n" : "user launch process association: FAIL\n");
    boot_diagnostics_write(s_user_launch_marker_ok ? "user launch stack marker: PASS\n" : "user launch stack marker: FAIL\n");
    boot_diagnostics_write(s_user_launch_arguments_ok ? "user launch arguments and environment: PASS\n" : "user launch arguments and environment: FAIL\n");
    diagnostic_write_u64("user launch actual vector: ", s_user_launch_actual_vector);
    diagnostic_write_u64("user launch actual RIP: ", s_user_launch_actual_ip);
    diagnostic_write_u64("user launch expected RIP: ", output.entry_point +
        (user_args_test_success_fault - user_args_test_start));
    for (size_t attempt = 0; attempt < 1000 && output.process->state() != ProcessState::Exited;
         ++attempt)
        scheduler_yield();
    bool retired = output.process->state() == ProcessState::Exited &&
                   scheduler_count_process_threads(output.process) == 0;
    boot_diagnostics_write(entered ? "user launch entry/context: PASS\n" :
                                     "user launch entry/context: FAIL\n");
    boot_diagnostics_write(retired ? "user launch thread retirement: PASS\n" :
                                     "user launch thread retirement: FAIL\n");
    bool released = output.process->release() == ProcessResult::Success;
    bool passed = entered && retired && released;
    ok &= passed;
    boot_diagnostics_write(passed ? "initial user thread ring3 execution: PASS\n" :
                                    "initial user thread ring3 execution: FAIL\n");
    return ok;
}

static bool run_user_fault_case(const char* name, uintptr_t target, bool write_target,
                                bool nx_code, uintptr_t extra_mapping = 0,
                                uintptr_t extra_frame = 0, uint64_t extra_flags = 0) {
    constexpr uintptr_t code_va = 0x00400000;
    Process* process = create_user_test_process();
    if (!process) return false;
    auto* thread = new Thread(process, reinterpret_cast<void(*)()>(code_va), true, name);
    if (!thread || thread->state == THREAD_DEAD) {
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }

    uintptr_t code_frame = pmm_alloc_page();
    if (!code_frame) {
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }
    uint64_t code_flags = VMM_PRESENT | VMM_USER;
    if (nx_code && nx_enabled()) code_flags |= VMM_NX;
    if (vmm_map_owned_page(process->page_table_phys, code_va, code_frame, code_flags) !=
        VmmMapResult::Success) {
        pmm_free_page(code_frame);
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }
    if (extra_frame && vmm_map_owned_page(process->page_table_phys, extra_mapping,
                                          extra_frame, extra_flags) != VmmMapResult::Success) {
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }

    uint8_t* code = reinterpret_cast<uint8_t*>(code_frame + pmm_hhdm_offset());
    for (size_t i = 0; i < PAGE_SIZE; ++i) code[i] = 0x90;
    if (!nx_code) {
        code[0] = 0x48; code[1] = 0xB8; // movabs target, %rax
        for (size_t i = 0; i < sizeof(uintptr_t); ++i)
            code[2 + i] = static_cast<uint8_t>(target >> (i * 8));
        if (write_target) {
            code[10] = 0xC6; code[11] = 0x00; code[12] = 0x01; // movb $1,(%rax)
            code[13] = 0x0F; code[14] = 0x0B; // ud2 if access unexpectedly succeeded
        } else {
            code[10] = 0x48; code[11] = 0x8B; code[12] = 0x18; // mov (%rax),%rbx
            code[13] = 0x0F; code[14] = 0x0B;
        }
    }

    uintptr_t root = process->page_table_phys;
    address_space_runtime_expect_fault(nx_code ? code_va : target, 14);
    if (scheduler_add_thread(thread) != SchedulerPublishResult::Success) {
        s_expect_user_fault = false;
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }
    for (size_t i = 0; i < 100 && !address_space_runtime_fault_seen(); ++i)
        scheduler_yield();
    if (!address_space_runtime_fault_seen()) {
        scheduler_terminate_thread(thread);
        scheduler_yield();
    }
    bool fault_ok = address_space_runtime_fault_seen() && address_space_runtime_fault_matches();
    s_expect_user_fault = false;
    bool torn_down = wait_address_space_destroyed(root);
    bool owned_frames_released = !pmm_is_allocated_page(code_frame) &&
        (!extra_frame || !pmm_is_allocated_page(extra_frame));
    bool process_exited = process->state() == ProcessState::Exited;
    bool process_released = release_test_process(process);
    bool passed = fault_ok && torn_down && owned_frames_released && process_exited && process_released;
    boot_diagnostics_write(passed ? "process isolation " : "process isolation ");
    boot_diagnostics_write(name);
    boot_diagnostics_write(passed ? ": PASS\n" : ": FAIL\n");
    return passed;
}

static bool run_user_syscall_pointer_test() {
    constexpr uintptr_t code_va = 0x00400000;
    constexpr uintptr_t result_va = 0x00600000;
    constexpr uintptr_t bad_user_va = 0x0000000000BAD000ULL;
    Process* process = create_user_test_process();
    if (!process) return false;
    auto* thread = new Thread(process, reinterpret_cast<void(*)()>(code_va), true,
                              "syscall-pointer-validation");
    uintptr_t code_frame = pmm_alloc_page();
    uintptr_t result_frame = pmm_alloc_page();
    if (!thread || thread->state == THREAD_DEAD || !code_frame || !result_frame) {
        if (code_frame) pmm_free_page(code_frame);
        if (result_frame) pmm_free_page(result_frame);
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }
    if (vmm_map_owned_page(process->page_table_phys, code_va, code_frame,
                           VMM_PRESENT | VMM_USER) != VmmMapResult::Success ||
        vmm_map_page(process->page_table_phys, result_va, result_frame,
                     VMM_PRESENT | VMM_WRITE | VMM_USER) != VmmMapResult::Success) {
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        if (pmm_is_allocated_page(code_frame)) pmm_free_page(code_frame);
        if (pmm_is_allocated_page(result_frame)) pmm_free_page(result_frame);
        return false;
    }

    uint8_t* code = reinterpret_cast<uint8_t*>(code_frame + pmm_hhdm_offset());
    for (size_t i = 0; i < PAGE_SIZE; ++i) code[i] = 0x90;
    size_t at = 0;
    auto emit = [&](uint8_t byte) { code[at++] = byte; };
    auto emit_imm64 = [&](uintptr_t value) {
        for (size_t i = 0; i < sizeof(uintptr_t); ++i)
            emit(static_cast<uint8_t>(value >> (i * 8)));
    };
    // write(1, invalid_user_pointer, 1), then record whether the syscall
    // returned -EFAULT. UD2 ends both paths and gives the controlled test fault.
    emit(0xB8); emit(1); emit(0); emit(0); emit(0);                 // mov $1,%eax
    emit(0xBF); emit(1); emit(0); emit(0); emit(0);                 // mov $1,%edi
    emit(0x48); emit(0xBE); emit_imm64(bad_user_va);               // movabs bad,%rsi
    emit(0xBA); emit(1); emit(0); emit(0); emit(0);                 // mov $1,%edx
    emit(0x0F); emit(0x05);                                         // syscall
    emit(0x48); emit(0x83); emit(0xF8); emit(0xF2);                 // cmp $-EFAULT,%rax
    emit(0x75); emit(15);                                           // jne failure path
    emit(0x48); emit(0xB8); emit_imm64(result_va);
    emit(0xC6); emit(0x00); emit(1);                                // success = 1
    emit(0x0F); emit(0x0B);                                         // ud2
    emit(0x48); emit(0xB8); emit_imm64(result_va);
    emit(0xC6); emit(0x00); emit(2);                                // failure = 2
    emit(0x0F); emit(0x0B);                                         // ud2
    *reinterpret_cast<volatile uint8_t*>(result_frame + pmm_hhdm_offset()) = 0;

    uintptr_t root = process->page_table_phys;
    address_space_runtime_expect_fault(0, 6);
    if (scheduler_add_thread(thread) != SchedulerPublishResult::Success) {
        s_expect_user_fault = false;
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        if (pmm_is_allocated_page(result_frame)) pmm_free_page(result_frame);
        return false;
    }
    for (size_t i = 0; i < 100 && !address_space_runtime_fault_seen(); ++i)
        scheduler_yield();
    if (!address_space_runtime_fault_seen()) {
        scheduler_terminate_thread(thread);
        scheduler_yield();
    }
    bool syscall_rejected_pointer =
        *reinterpret_cast<volatile uint8_t*>(result_frame + pmm_hhdm_offset()) == 1;
    bool expected_end = address_space_runtime_fault_seen() &&
                        address_space_runtime_fault_matches();
    s_expect_user_fault = false;
    bool torn_down = wait_address_space_destroyed(root);
    bool borrowed_frame_preserved = pmm_is_allocated_page(result_frame);
    pmm_free_page(result_frame); // borrowed user mapping must not own this frame
    bool process_released = release_test_process(process);
    bool passed = syscall_rejected_pointer && expected_end && torn_down &&
                  borrowed_frame_preserved && process_released;
    boot_diagnostics_write(passed ? "process isolation syscall-pointer-validation: PASS\n" :
                                     "process isolation syscall-pointer-validation: FAIL\n");
    return passed;
}

static bool run_user_mmap_test(bool verify_munmap) {
    constexpr uintptr_t code_va = 0x00400000;
    constexpr uintptr_t mapped_va = 0x100000000ULL;
    Process* process = create_user_test_process();
    if (!process) return false;
    auto* thread = new Thread(process, reinterpret_cast<void(*)()>(code_va), true,
                              verify_munmap ? "mmap-munmap" : "mmap-read-only");
    uintptr_t code_frame = pmm_alloc_page();
    if (!thread || thread->state == THREAD_DEAD || !code_frame) {
        if (code_frame) pmm_free_page(code_frame);
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }
    if (vmm_map_owned_page(process->page_table_phys, code_va, code_frame,
                           VMM_PRESENT | VMM_USER) != VmmMapResult::Success) {
        pmm_free_page(code_frame);
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }

    uint8_t* code = reinterpret_cast<uint8_t*>(code_frame + pmm_hhdm_offset());
    for (size_t i = 0; i < PAGE_SIZE; ++i) code[i] = 0x90;
    size_t at = 0;
    auto emit = [&](uint8_t byte) { code[at++] = byte; };
    // mmap(NULL, 4096, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
    emit(0xB8); emit(9); emit(0); emit(0); emit(0);                // mov $9,%eax
    emit(0x31); emit(0xFF);                                        // xor %edi,%edi
    emit(0xBE); emit(0); emit(0x10); emit(0); emit(0);            // mov $4096,%esi
    emit(0xBA); emit(verify_munmap ? 3 : 1); emit(0); emit(0); emit(0); // prot
    emit(0x41); emit(0xBA); emit(0x22); emit(0); emit(0); emit(0); // flags
    emit(0x41); emit(0xB8); emit(0xFF); emit(0xFF); emit(0xFF); emit(0xFF); // fd=-1
    emit(0x45); emit(0x31); emit(0xC9);                            // xor %r9d,%r9d
    emit(0x0F); emit(0x05);                                        // syscall
    if (verify_munmap) {
        emit(0x48); emit(0x89); emit(0xC3);                        // mov %rax,%rbx
        emit(0xC6); emit(0x00); emit(0x5A);                        // store succeeds
        emit(0x48); emit(0x89); emit(0xDF);                        // mov %rbx,%rdi
        emit(0xBE); emit(0); emit(0x10); emit(0); emit(0);         // length=4096
        emit(0xB8); emit(11); emit(0); emit(0); emit(0);           // munmap
        emit(0x0F); emit(0x05);
        emit(0x48); emit(0x8B); emit(0x0B);                        // unmapped load faults
        emit(0x0F); emit(0x0B);
    } else {
        emit(0xC6); emit(0x00); emit(1);                           // read-only write faults
        emit(0x0F); emit(0x0B);
    }

    uintptr_t root = process->page_table_phys;
    uintptr_t expected_address = mapped_va;
    address_space_runtime_expect_fault(expected_address, 14);
    if (scheduler_add_thread(thread) != SchedulerPublishResult::Success) {
        s_expect_user_fault = false;
        delete thread;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }
    for (size_t i = 0; i < 100 && !address_space_runtime_fault_seen(); ++i)
        scheduler_yield();
    if (!address_space_runtime_fault_seen()) {
        scheduler_terminate_thread(thread);
        scheduler_yield();
    }
    bool fault_ok = address_space_runtime_fault_seen() && address_space_runtime_fault_matches();
    uint64_t leaf_flags = 0;
    bool mapping_owned = false;
    bool mapping_found = address_space_runtime_fault_mapping(&leaf_flags, &mapping_owned);
    s_expect_user_fault = false;
    bool torn_down = wait_address_space_destroyed(root);
    bool permissions_ok = verify_munmap
        ? !mapping_found
        : (mapping_found && mapping_owned && (leaf_flags & (VMM_PRESENT | VMM_USER | VMM_NX)) ==
              (VMM_PRESENT | VMM_USER | VMM_NX) && !(leaf_flags & VMM_WRITE));
    bool process_released = release_test_process(process);
    bool passed = fault_ok && torn_down && permissions_ok &&
                  !pmm_is_allocated_page(code_frame) && process_released;
    boot_diagnostics_write(passed ? "process isolation " : "process isolation ");
    boot_diagnostics_write(verify_munmap ? "mmap-munmap" : "mmap-permissions");
    boot_diagnostics_write(passed ? ": PASS\n" : ": FAIL\n");
    return passed;
}

bool run_address_space_runtime_test() {
    boot_diagnostics_write("address-space test: begin\n");
    bool ok = run_empty_process_creation_test();
    ok &= run_initial_user_thread_test();
    bool timeout_recovered = smp_test_timeout_recovery();
    ok &= timeout_recovered;
    boot_diagnostics_write(timeout_recovered ? "SMP timeout recovery: PASS\n" : "SMP timeout recovery: FAIL\n");
    const uintptr_t kernel_root = vmm_get_kernel_pml4();
    constexpr uintptr_t private_va = 0x0000000012345000ULL;
    constexpr uintptr_t only_a_va = 0x0000000012445000ULL;
    constexpr uintptr_t shared_kernel_va = 0xFFFFD00000001000ULL;
    constexpr uintptr_t mmio_va = 0xFFFFD10000001000ULL;

    uintptr_t root_a = vmm_create_address_space();
    uintptr_t root_b = vmm_create_address_space();
    uintptr_t owned_frame = pmm_alloc_page();
    uintptr_t borrowed_frame = pmm_alloc_page();
    uintptr_t shared_frame = pmm_alloc_page();
    uintptr_t mmio_frame = pmm_alloc_page();
    if (!root_a || !root_b || !owned_frame || !borrowed_frame || !shared_frame || !mmio_frame) {
        boot_diagnostics_write("address-space test: FAIL allocating roots or frames\n");
        return false;
    }

    ok &= vmm_map_owned_page(root_a, private_va, owned_frame,
                             VMM_PRESENT | VMM_WRITE | VMM_USER) == VmmMapResult::Success;
    ok &= vmm_map_page(root_a, only_a_va, borrowed_frame,
                       VMM_PRESENT | VMM_USER) == VmmMapResult::Success;
    ok &= vmm_map_page(root_b, private_va, borrowed_frame,
                       VMM_PRESENT | VMM_USER) == VmmMapResult::Success;
    ok &= vmm_map_page(kernel_root, shared_kernel_va, shared_frame,
                       VMM_PRESENT | VMM_WRITE) == VmmMapResult::Success;
    ok &= vmm_map_mmio_page(mmio_va, mmio_frame) == VmmMapResult::Success;

    VmmMappingInfo info = {};
    ok &= vmm_switch_address_space(root_a) == VmmAddressSpaceResult::Success;
    ok &= vmm_query_mapping(private_va, &info) && info.physical_page == owned_frame &&
          info.ownership == VmmMappingInfo::Ownership::Owned;
    ok &= vmm_query_mapping(only_a_va, &info) && info.physical_page == borrowed_frame;
    ok &= !vmm_query_mapping(0x0000000012446000ULL, &info);
    ok &= vmm_query_mapping(shared_kernel_va, &info) && info.physical_page == shared_frame;
    ok &= vmm_query_mapping(mmio_va, &info) && info.physical_page == mmio_frame &&
          info.ownership == VmmMappingInfo::Ownership::Mmio;

    // Keep the same user root active on all APs while BSP unmaps and remaps a
    // warmed translation. The second read detects a stale AP TLB entry.
    constexpr uint64_t value_before = 0x4452414B4F535031ULL;
    constexpr uint64_t value_after = 0x4452414B4F535032ULL;
    *reinterpret_cast<volatile uint64_t*>(owned_frame + pmm_hhdm_offset()) = value_before;
    uintptr_t replacement_frame = pmm_alloc_page();
    if (!replacement_frame || smp_online_cpu_count() < 2) {
        ok = false;
        boot_diagnostics_write("address-space SMP test: FAIL; fewer than two CPUs or no frame\n");
    }
    uint32_t ap_references = 0;
    bool retain_ok = true;
    for (uint32_t i = 1; i < smp_online_cpu_count(); ++i) {
        if (vmm_retain_address_space(root_a) != VmmAddressSpaceResult::Success) {
            retain_ok = false;
            break;
        }
        ++ap_references;
    }
    bool ap_switched = retain_ok && ap_references == smp_online_cpu_count() - 1 &&
                       smp_test_switch_address_space(root_a);
    bool bsp_switched_away = vmm_switch_address_space(kernel_root) == VmmAddressSpaceResult::Success;
    bool read_before_ok = ap_switched && smp_test_read_virtual(private_va, value_before);
    bool remote_active_rejected = vmm_destroy_address_space(root_a) == VmmAddressSpaceResult::ActiveAddressSpace;
    ok &= retain_ok && ap_switched && bsp_switched_away && read_before_ok && remote_active_rejected;
    boot_diagnostics_write(retain_ok ? "address-space SMP retain: PASS\n" : "address-space SMP retain: FAIL\n");
    boot_diagnostics_write(ap_switched ? "address-space AP switch: PASS\n" : "address-space AP switch: FAIL\n");
    boot_diagnostics_write(bsp_switched_away ? "address-space BSP switch: PASS\n" : "address-space BSP switch: FAIL\n");
    boot_diagnostics_write(read_before_ok ? "address-space AP initial read: PASS\n" : "address-space AP initial read: FAIL\n");
    boot_diagnostics_write(remote_active_rejected ? "address-space remote-active destroy guard: PASS\n" : "address-space remote-active destroy guard: FAIL\n");
    if (replacement_frame) {
        *reinterpret_cast<volatile uint64_t*>(replacement_frame + pmm_hhdm_offset()) = value_after;
        bool unmapped = vmm_unmap_page(root_a, private_va) == VmmUnmapResult::Success;
        bool old_reclaimed = !pmm_is_allocated_page(owned_frame);
        bool remapped = vmm_map_owned_page(root_a, private_va, replacement_frame,
                                 VMM_PRESENT | VMM_WRITE | VMM_USER) == VmmMapResult::Success;
        bool read_after_ok = remapped && smp_test_read_virtual(private_va, value_after);
        ok &= unmapped && old_reclaimed && remapped && read_after_ok;
        boot_diagnostics_write(unmapped ? "address-space AP unmap: PASS\n" : "address-space AP unmap: FAIL\n");
        boot_diagnostics_write(old_reclaimed ? "address-space old frame release: PASS\n" : "address-space old frame release: FAIL\n");
        boot_diagnostics_write(remapped ? "address-space AP remap: PASS\n" : "address-space AP remap: FAIL\n");
        boot_diagnostics_write(read_after_ok ? "address-space AP post-shootdown read: PASS\n" : "address-space AP post-shootdown read: FAIL\n");
    }
    bool aps_returned = smp_test_switch_address_space(kernel_root);
    ok &= aps_returned;
    boot_diagnostics_write(aps_returned ? "address-space AP return: PASS\n" : "address-space AP return: FAIL\n");
    while (ap_references) {
        --ap_references;
        ok &= vmm_release_address_space(root_a) == VmmAddressSpaceResult::Success;
    }
    if (replacement_frame && !pmm_is_allocated_page(replacement_frame))
        ok = false;

    ok &= vmm_switch_address_space(root_b) == VmmAddressSpaceResult::Success;
    ok &= vmm_query_mapping(private_va, &info) && info.physical_page == borrowed_frame &&
          info.ownership == VmmMappingInfo::Ownership::Borrowed;
    ok &= !vmm_query_mapping(only_a_va, &info);
    ok &= vmm_query_mapping(shared_kernel_va, &info) && info.physical_page == shared_frame;
    ok &= vmm_unmap_page(root_b, private_va) == VmmUnmapResult::Success;
    ok &= vmm_switch_address_space(kernel_root) == VmmAddressSpaceResult::Success;

    ok &= vmm_destroy_address_space(root_a) == VmmAddressSpaceResult::Success;
    ok &= vmm_destroy_address_space(root_b) == VmmAddressSpaceResult::Success;
    ok &= !pmm_is_allocated_page(owned_frame);
    if (replacement_frame) ok &= !pmm_is_allocated_page(replacement_frame);
    ok &= pmm_is_allocated_page(borrowed_frame);
    ok &= vmm_get_phys(shared_kernel_va) == shared_frame;
    ok &= vmm_get_phys(mmio_va) == mmio_frame;

    ok &= vmm_unmap_page(kernel_root, shared_kernel_va) == VmmUnmapResult::Success;
    ok &= vmm_unmap_page(kernel_root, mmio_va) == VmmUnmapResult::Success;
    ok &= pmm_is_allocated_page(shared_frame) && pmm_is_allocated_page(mmio_frame);
    pmm_free_page(borrowed_frame);
    pmm_free_page(shared_frame);
    pmm_free_page(mmio_frame);

    // Exercise scheduler-owned references with two threads sharing one process root.
    Process* process = create_user_test_process();
    if (!process) return false;
    uintptr_t process_root = process->page_table_phys;
    Thread* first = new Thread(process, runtime_test_thread_entry, false, "address-space-test-a");
    Thread* second = new Thread(process, runtime_test_thread_entry, false, "address-space-test-b");
    if (!first || !second || first->state == THREAD_DEAD || second->state == THREAD_DEAD) {
        delete first;
        delete second;
        if (process->release() != ProcessResult::Success) return false;
        boot_diagnostics_write("address-space test: FAIL creating shared-thread process\n");
        return false;
    }
    bool first_published = scheduler_add_thread(first) == SchedulerPublishResult::Success;
    bool second_published = first_published &&
        scheduler_add_thread(second) == SchedulerPublishResult::Success;
    if (!first_published || !second_published) {
        if (first_published) scheduler_terminate_thread(first);
        if (second_published) scheduler_terminate_thread(second);
        while (scheduler_count_process_threads(process)) scheduler_yield();
        if (!first_published) delete first;
        if (!second_published) delete second;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }
    bool refs_block_destroy = vmm_destroy_address_space(process->page_table_phys) ==
                              VmmAddressSpaceResult::AddressSpaceInUse;
    scheduler_terminate_thread(first);
    scheduler_terminate_thread(second);
    scheduler_yield();
    bool dead_threads_released = vmm_retain_address_space(process_root) ==
                                 VmmAddressSpaceResult::InvalidAddressSpace;
    bool process_released = process->release() == ProcessResult::Success;
    ok &= refs_block_destroy && dead_threads_released && process_released;

    // Run real CPL3 instruction streams and require controlled #PFs at the
    // expected addresses. The exception handler records these test faults and
    // retires the user thread instead of panicking the kernel.
    VmmMappingInfo kernel_info = {};
    uintptr_t kernel_page_table_va = pmm_hhdm_offset() + kernel_root;
    bool kernel_mapping_is_supervisor = vmm_query_mapping(kernel_page_table_va, &kernel_info) &&
                                        !(kernel_info.flags & VMM_USER);
    ok &= kernel_mapping_is_supervisor && run_user_fault_case(
        "kernel-read-protection", kernel_page_table_va, false, false);

    uintptr_t readonly_frame = pmm_alloc_page();
    bool readonly_test = readonly_frame && run_user_fault_case(
        "read-only-write-protection", 0x00600000, true, false, 0x00600000,
        readonly_frame, VMM_PRESENT | VMM_USER);
    if (readonly_frame && pmm_is_allocated_page(readonly_frame)) pmm_free_page(readonly_frame);
    ok &= readonly_test;

    uintptr_t other_root = vmm_create_address_space();
    uintptr_t other_private_frame = pmm_alloc_page();
    bool other_mapping = other_root && other_private_frame &&
        vmm_map_owned_page(other_root, 0x00500000, other_private_frame,
                           VMM_PRESENT | VMM_USER) == VmmMapResult::Success;
    bool cross_process_test = other_mapping && run_user_fault_case(
        "private-process-mapping", 0x00500000, false, false);
    ok &= cross_process_test;
    if (other_root) {
        if (other_mapping) ok &= pmm_is_allocated_page(other_private_frame);
        ok &= vmm_destroy_address_space(other_root) == VmmAddressSpaceResult::Success;
        if (other_mapping) ok &= !pmm_is_allocated_page(other_private_frame);
        else if (other_private_frame && pmm_is_allocated_page(other_private_frame))
            pmm_free_page(other_private_frame);
    } else if (other_private_frame && pmm_is_allocated_page(other_private_frame))
        pmm_free_page(other_private_frame);

    if (nx_enabled()) {
        ok &= run_user_fault_case("NX-data-page", 0, false, true);
    } else {
        boot_diagnostics_write("process isolation NX test: SKIP (CPU has no NX support)\n");
    }
    ok &= run_user_syscall_pointer_test();
    ok &= run_user_mmap_test(false);
    ok &= run_user_mmap_test(true);

    // Cross the old 128-root boundary in the running kernel, map separate
    // pages at one VA, and remove roots from both registry chunks.
    static uintptr_t stress_roots[260];
    size_t stress_count = 0;
    for (; stress_count < 260; ++stress_count) {
        stress_roots[stress_count] = vmm_create_address_space();
        if (!stress_roots[stress_count]) break;
    }
    uintptr_t stress_frame_a = pmm_alloc_page();
    uintptr_t stress_frame_b = pmm_alloc_page();
    constexpr uintptr_t stress_va = 0x0000000055555000ULL;
    bool stress_ok = stress_count == 260 && stress_frame_a && stress_frame_b;
    if (stress_count != 260) boot_diagnostics_write("address-space registry stress: root allocation stopped early\n");
    if (!stress_frame_a || !stress_frame_b) boot_diagnostics_write("address-space registry stress: frame allocation failed\n");
    if (stress_ok) {
        stress_ok &= vmm_map_owned_page(stress_roots[0], stress_va, stress_frame_a,
            VMM_PRESENT | VMM_USER) == VmmMapResult::Success;
        stress_ok &= vmm_map_owned_page(stress_roots[129], stress_va, stress_frame_b,
            VMM_PRESENT | VMM_USER) == VmmMapResult::Success;
        stress_ok &= vmm_retain_address_space(stress_roots[200]) == VmmAddressSpaceResult::Success;
        stress_ok &= vmm_destroy_address_space(stress_roots[200]) == VmmAddressSpaceResult::AddressSpaceInUse;
        stress_ok &= vmm_release_address_space(stress_roots[200]) == VmmAddressSpaceResult::Success;
        stress_ok &= vmm_switch_address_space(stress_roots[0]) == VmmAddressSpaceResult::Success;
        stress_ok &= vmm_query_mapping(stress_va, &info) && info.physical_page == stress_frame_a;
        stress_ok &= vmm_switch_address_space(stress_roots[129]) == VmmAddressSpaceResult::Success;
        stress_ok &= vmm_query_mapping(stress_va, &info) && info.physical_page == stress_frame_b;
        stress_ok &= vmm_switch_address_space(kernel_root) == VmmAddressSpaceResult::Success;
    }
    if (!stress_ok) boot_diagnostics_write("address-space registry stress: mapping/reference verification failed\n");
    for (size_t i = 0; i < stress_count; i += 2)
        stress_ok &= vmm_destroy_address_space(stress_roots[i]) == VmmAddressSpaceResult::Success;
    for (size_t i = 1; i < stress_count; i += 2)
        stress_ok &= vmm_destroy_address_space(stress_roots[i]) == VmmAddressSpaceResult::Success;
    if (stress_frame_a) {
        if (pmm_is_allocated_page(stress_frame_a)) pmm_free_page(stress_frame_a);
        stress_ok &= !pmm_is_allocated_page(stress_frame_a);
    }
    if (stress_frame_b) {
        if (pmm_is_allocated_page(stress_frame_b)) pmm_free_page(stress_frame_b);
        stress_ok &= !pmm_is_allocated_page(stress_frame_b);
    }
    ok &= stress_ok;
    boot_diagnostics_write(stress_ok ? "address-space registry stress: PASS (260 roots)\n"
                                     : "address-space registry stress: FAIL\n");

    boot_diagnostics_write(ok ? "address-space test: PASS\n" : "address-space test: FAIL\n");
    return ok;
}

bool run_smp_kernel_scheduler_runtime_test() {
    uint32_t online_cpus = smp_online_cpu_count();
    bool all_schedulers_enabled = online_cpus >= 2 &&
        scheduler_enabled_processor_count() == online_cpus;
    if (!all_schedulers_enabled) {
        boot_diagnostics_write("SMP scheduler CPU initialization: FAIL\n");
        return false;
    }
    boot_diagnostics_write("SMP scheduler CPU initialization: PASS\n");
    Process* process = new Process();
    Thread* first = process ? new Thread(process, smp_kernel_counter_thread, false,
                                         "smp-kernel-a") : nullptr;
    Thread* second = process ? new Thread(process, smp_kernel_counter_thread, false,
                                          "smp-kernel-b") : nullptr;
    bool ok = process && first && second && first->state != THREAD_DEAD &&
              second->state != THREAD_DEAD;
    if (!ok) {
        delete first;
        delete second;
        if (process && process->release() != ProcessResult::Success) return false;
        boot_diagnostics_write("SMP concurrent kernel threads: FAIL (setup)\n");
        return false;
    }
    s_smp_kernel_counter = 0;
    bool first_published = scheduler_add_thread(first) == SchedulerPublishResult::Success;
    bool second_published = first_published &&
        scheduler_add_thread(second) == SchedulerPublishResult::Success;
    bool enrolled = first_published && second_published;
    if (!enrolled) {
        if (first_published) scheduler_terminate_thread(first);
        if (second_published) scheduler_terminate_thread(second);
        while (scheduler_count_process_threads(process)) scheduler_yield();
        if (!first_published) delete first;
        if (!second_published) delete second;
        if (process->release() != ProcessResult::Success) return false;
        return false;
    }
    uint32_t lo, hi;
    uint64_t start, now;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    start = (static_cast<uint64_t>(hi) << 32) | lo;
    bool concurrent = false;
    bool migrated = false;
    while (enrolled) {
        concurrent = scheduler_count_running_process_cpus(process) >= 2;
        migrated = scheduler_thread_migration_count(first) +
                   scheduler_thread_migration_count(second) > 0;
        if (concurrent && migrated &&
            __atomic_load_n(&s_smp_kernel_counter, __ATOMIC_RELAXED) != 0)
            break;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        now = (static_cast<uint64_t>(hi) << 32) | lo;
        if (now - start > 5000000000ULL) break;
        asm volatile("pause");
    }
    migrated |= scheduler_thread_migration_count(first) +
                scheduler_thread_migration_count(second) > 0;
    if (first->scheduler_enrolled) scheduler_terminate_thread(first);
    if (second->scheduler_enrolled) scheduler_terminate_thread(second);
    if (!first->scheduler_enrolled) delete first;
    if (!second->scheduler_enrolled) delete second;
    bool reclaimed = false;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    start = (static_cast<uint64_t>(hi) << 32) | lo;
    while (scheduler_count_process_threads(process) != 0) {
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        now = (static_cast<uint64_t>(hi) << 32) | lo;
        if (now - start > 5000000000ULL) break;
        asm volatile("pause");
    }
    reclaimed = scheduler_count_process_threads(process) == 0;
    bool process_released = process->release() == ProcessResult::Success;
    ok = enrolled && concurrent && migrated && reclaimed && process_released;
    boot_diagnostics_write(ok ? "SMP concurrent kernel threads: PASS\n"
                              : "SMP concurrent kernel threads: FAIL\n");
    boot_diagnostics_write(migrated ? "SMP thread migration: PASS\n"
                                    : "SMP thread migration: FAIL\n");
    return ok;
}

bool run_smp_user_scheduler_runtime_test() {
    if (smp_online_cpu_count() < 2) {
        boot_diagnostics_write("SMP user scheduling: FAIL (fewer than 2 CPUs)\n");
        return false;
    }

    constexpr uintptr_t code_va = 0x00400000;
    constexpr uintptr_t data_va = 0x00401000;
    Process* process = create_user_test_process();
    if (!process) return false;
    uintptr_t root = process->page_table_phys;
    uintptr_t code_frame = pmm_alloc_page();
    uintptr_t data_frame = pmm_alloc_page();
    bool ok = code_frame && data_frame;
    if (ok) {
        // User code performs getpid through SYSCALL and atomically increments
        // a shared counter forever. Timer interrupts must preempt it on each CPU.
        auto* code = reinterpret_cast<volatile uint8_t*>(code_frame + pmm_hhdm_offset());
        uint8_t program[] = {
            0x48, 0xBB, // movabs rbx, data_va
            0x00, 0x10, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xB8, 0x27, 0x00, 0x00, 0x00, // mov eax, SYS_getpid
            0x0F, 0x05,                   // syscall
            0xF0, 0x48, 0xFF, 0x03,       // lock inc qword [rbx]
            0xEB, 0xF3                    // jmp to SYS_getpid
        };
        for (size_t i = 0; i < sizeof(program); ++i) code[i] = program[i];
        auto* counter = reinterpret_cast<volatile uint64_t*>(data_frame + pmm_hhdm_offset());
        *counter = 0;
        bool code_mapped = vmm_map_owned_page(root, code_va, code_frame,
                                 VMM_PRESENT | VMM_USER) == VmmMapResult::Success;
        bool data_mapped = code_mapped && vmm_map_owned_page(root, data_va, data_frame,
                                 VMM_PRESENT | VMM_WRITE | VMM_USER |
                                     (nx_enabled() ? VMM_NX : 0)) == VmmMapResult::Success;
        ok = code_mapped && data_mapped;
        if (!code_mapped && code_frame && pmm_is_allocated_page(code_frame))
            pmm_free_page(code_frame);
        if (!data_mapped && data_frame && pmm_is_allocated_page(data_frame))
            pmm_free_page(data_frame);
    }

    auto* first = ok ? new Thread(process, reinterpret_cast<void(*)()>(code_va), true,
                                 "smp-user-a") : nullptr;
    auto* second = ok ? new Thread(process, reinterpret_cast<void(*)()>(code_va), true,
                                  "smp-user-b") : nullptr;
    ok &= first && second && first->state != THREAD_DEAD && second->state != THREAD_DEAD;
    if (!ok) {
        delete first;
        delete second;
        if (process->release() != ProcessResult::Success) return false;
        boot_diagnostics_write("SMP user scheduling: FAIL (setup)\n");
        return false;
    }
    uint64_t ap_syscalls_before = smp_ap_syscall_count();
    bool first_published = ok &&
        scheduler_add_thread(first) == SchedulerPublishResult::Success;
    bool second_published = first_published &&
        scheduler_add_thread(second) == SchedulerPublishResult::Success;
    ok &= first_published && second_published;
    if (!ok) {
        if (first_published) scheduler_terminate_thread(first);
        if (second_published) scheduler_terminate_thread(second);
        while (scheduler_count_process_threads(process)) scheduler_yield();
        if (!first_published) delete first;
        if (!second_published) delete second;
        if (process->release() != ProcessResult::Success) return false;
        boot_diagnostics_write("SMP user scheduling: FAIL (enqueue)\n");
        return false;
    }

    uint64_t tsc_start, tsc_end;
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    tsc_start = (static_cast<uint64_t>(hi) << 32) | lo;
    bool concurrent = false;
    bool syscall_progress = false;
    while (ok) {
        concurrent = scheduler_count_running_process_cpus(process) >= 2;
        auto* counter = reinterpret_cast<volatile uint64_t*>(data_frame + pmm_hhdm_offset());
        syscall_progress = *counter > 0 && smp_ap_syscall_count() > ap_syscalls_before;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        tsc_end = (static_cast<uint64_t>(hi) << 32) | lo;
        if (concurrent && syscall_progress) break;
        if (tsc_end - tsc_start > 5000000000ULL) break;
        asm volatile("pause");
    }

    if (first && first->scheduler_enrolled) scheduler_terminate_thread(first);
    if (second && second->scheduler_enrolled) scheduler_terminate_thread(second);
    if (!first || !first->scheduler_enrolled) delete first;
    if (!second || !second->scheduler_enrolled) delete second;

    bool reclaimed = false;
    if (process->is_user) {
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        tsc_start = (static_cast<uint64_t>(hi) << 32) | lo;
        for (;;) {
            VmmAddressSpaceResult retain = vmm_retain_address_space(root);
            if (retain == VmmAddressSpaceResult::InvalidAddressSpace) {
                reclaimed = true;
                break;
            }
            if (retain == VmmAddressSpaceResult::Success)
                (void)vmm_release_address_space(root);
            asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
            tsc_end = (static_cast<uint64_t>(hi) << 32) | lo;
            if (tsc_end - tsc_start > 5000000000ULL) break;
            asm volatile("pause");
        }
    }
    ok &= concurrent && syscall_progress && reclaimed;
    ok &= process->release() == ProcessResult::Success;
    boot_diagnostics_write(concurrent ? "SMP shared address-space execution: PASS\n"
                                      : "SMP shared address-space execution: FAIL\n");
    boot_diagnostics_write(syscall_progress ? "SMP AP syscall progress: PASS\n"
                                            : "SMP AP syscall progress: FAIL\n");
    boot_diagnostics_write(reclaimed ? "SMP concurrent process teardown: PASS\n"
                                     : "SMP concurrent process teardown: FAIL\n");
    return ok;
}

bool run_smp_independent_user_runtime_test() {
    constexpr uintptr_t code_va = 0x00400000;
    constexpr uintptr_t data_va = 0x00401000;
    Process* first_process = create_user_test_process();
    Process* second_process = create_user_test_process();
    if (!first_process || !second_process) {
        if (first_process && first_process->release() != ProcessResult::Success) return false;
        if (second_process && second_process->release() != ProcessResult::Success) return false;
        return false;
    }
    uintptr_t first_root = first_process->page_table_phys;
    uintptr_t second_root = second_process->page_table_phys;
    uintptr_t first_code = pmm_alloc_page();
    uintptr_t first_data = pmm_alloc_page();
    uintptr_t second_code = pmm_alloc_page();
    uintptr_t second_data = pmm_alloc_page();
    bool setup = first_code && first_data && second_code && second_data;
    if (setup) {
        install_smp_user_counter_program(first_code);
        install_smp_user_counter_program(second_code);
        *reinterpret_cast<volatile uint64_t*>(first_data + pmm_hhdm_offset()) = 0;
        *reinterpret_cast<volatile uint64_t*>(second_data + pmm_hhdm_offset()) = 0;
        uint64_t data_flags = VMM_PRESENT | VMM_WRITE | VMM_USER |
                              (nx_enabled() ? VMM_NX : 0);
        setup = vmm_map_owned_page(first_root, code_va, first_code,
                                   VMM_PRESENT | VMM_USER) == VmmMapResult::Success &&
                vmm_map_owned_page(first_root, data_va, first_data,
                                   data_flags) == VmmMapResult::Success &&
                vmm_map_owned_page(second_root, code_va, second_code,
                                   VMM_PRESENT | VMM_USER) == VmmMapResult::Success &&
                vmm_map_owned_page(second_root, data_va, second_data,
                                   data_flags) == VmmMapResult::Success;
    }
    Thread* first = setup ? new Thread(first_process,
        reinterpret_cast<void(*)()>(code_va), true, "smp-process-a") : nullptr;
    Thread* second = setup ? new Thread(second_process,
        reinterpret_cast<void(*)()>(code_va), true, "smp-process-b") : nullptr;
    setup &= first && second && first->state != THREAD_DEAD && second->state != THREAD_DEAD;
    if (!setup) {
        delete first;
        delete second;
        if (first_process->release() != ProcessResult::Success ||
            second_process->release() != ProcessResult::Success)
            return false;
        if (first_code && pmm_is_allocated_page(first_code)) pmm_free_page(first_code);
        if (first_data && pmm_is_allocated_page(first_data)) pmm_free_page(first_data);
        if (second_code && pmm_is_allocated_page(second_code)) pmm_free_page(second_code);
        if (second_data && pmm_is_allocated_page(second_data)) pmm_free_page(second_data);
        boot_diagnostics_write("SMP independent address spaces: FAIL (setup)\n");
        return false;
    }

    bool first_published = scheduler_add_thread(first) == SchedulerPublishResult::Success;
    bool second_published = first_published &&
        scheduler_add_thread(second) == SchedulerPublishResult::Success;
    bool enrolled = first_published && second_published;
    if (!enrolled) {
        if (first_published) scheduler_terminate_thread(first);
        if (second_published) scheduler_terminate_thread(second);
        while (scheduler_count_process_threads(first_process) ||
               scheduler_count_process_threads(second_process)) scheduler_yield();
        if (!first_published) delete first;
        if (!second_published) delete second;
        if (first_process->release() != ProcessResult::Success ||
            second_process->release() != ProcessResult::Success) return false;
        return false;
    }
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t start = (static_cast<uint64_t>(hi) << 32) | lo;
    bool concurrent = false;
    bool progress = false;
    while (enrolled) {
        concurrent = scheduler_count_running_process_cpus(first_process) >= 1 &&
                     scheduler_count_running_process_cpus(second_process) >= 1;
        progress = *reinterpret_cast<volatile uint64_t*>(first_data + pmm_hhdm_offset()) > 0 &&
                   *reinterpret_cast<volatile uint64_t*>(second_data + pmm_hhdm_offset()) > 0;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t now = (static_cast<uint64_t>(hi) << 32) | lo;
        if (concurrent && progress) break;
        if (now - start > 5000000000ULL) break;
        asm volatile("pause");
    }
    if (first->scheduler_enrolled) scheduler_terminate_thread(first);
    if (second->scheduler_enrolled) scheduler_terminate_thread(second);
    bool reclaimed_first = false, reclaimed_second = false;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    start = (static_cast<uint64_t>(hi) << 32) | lo;
    while (!reclaimed_first || !reclaimed_second) {
        if (!reclaimed_first) {
            VmmAddressSpaceResult r = vmm_retain_address_space(first_root);
            reclaimed_first = r == VmmAddressSpaceResult::InvalidAddressSpace;
            if (r == VmmAddressSpaceResult::Success)
                (void)vmm_release_address_space(first_root);
        }
        if (!reclaimed_second) {
            VmmAddressSpaceResult r = vmm_retain_address_space(second_root);
            reclaimed_second = r == VmmAddressSpaceResult::InvalidAddressSpace;
            if (r == VmmAddressSpaceResult::Success)
                (void)vmm_release_address_space(second_root);
        }
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t now = (static_cast<uint64_t>(hi) << 32) | lo;
        if (now - start > 5000000000ULL) break;
        asm volatile("pause");
    }
    bool processes_released = first_process->release() == ProcessResult::Success &&
                              second_process->release() == ProcessResult::Success;
    bool ok = enrolled && concurrent && progress && reclaimed_first && reclaimed_second &&
              processes_released;
    boot_diagnostics_write(ok ? "SMP independent address spaces: PASS\n"
                              : "SMP independent address spaces: FAIL\n");
    return ok;
}
