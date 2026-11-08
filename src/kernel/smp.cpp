#include "kernel/smp.hpp"

#include "kernel/limine_requests.hpp"
#include "kernel/gdt.hpp"
#include "kernel/idt.hpp"
#include "cpu.hpp"
#include "drivers/lapic.hpp"
#include "kernel/boot_diagnostics.hpp"
#include "memory/vmm.hpp"

static constexpr uint32_t SMP_MAX_CPUS = 256;
static constexpr uint8_t SMP_IPI_VECTOR = 241;
static constexpr uint64_t SMP_TIMEOUT_TICKS = 1000000000ULL;

enum class SmpCommand : uint8_t { None, InvalidatePage, FlushNonGlobal, SwitchAddressSpace, ReadVirtual, TestNoAck };

struct SmpCpu {
    uint32_t processor_id;
    uint32_t lapic_id;
    bool bootstrap;
    volatile bool online;
    volatile uint64_t request_sequence;
    volatile uint64_t acknowledged_sequence;
    volatile SmpCommand command;
    volatile uintptr_t address_space;
    volatile uintptr_t active_root;
    volatile uintptr_t virtual_address;
    volatile uint64_t read_value;
};

static SmpCpu s_cpus[SMP_MAX_CPUS] = {};
static uint32_t s_cpu_count = 0;
static uint32_t s_bootstrap_lapic_id = UINT32_MAX;
static volatile bool s_initialized = false;
static volatile bool s_sender_lock = false;
static uint64_t s_next_sequence = 0;

static uint64_t read_tsc() {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<uint64_t>(high) << 32) | low;
}

static SmpCpu* cpu_by_lapic(uint32_t lapic_id) {
    for (uint32_t i = 0; i < s_cpu_count; ++i)
        if (s_cpus[i].online && s_cpus[i].lapic_id == lapic_id) return &s_cpus[i];
    return nullptr;
}

static uint32_t current_lapic_id() {
    return g_lapic ? (g_lapic->read(LAPIC_ID) >> 24) : UINT32_MAX;
}

static void ap_entry(limine_mp_info* info) {
    if (!info || info->processor_id >= s_cpu_count || !g_lapic || !g_gdt || !g_idt) {
        for (;;) asm volatile("cli; hlt");
    }

    // Limine enters APs in long mode. Move to the kernel's page tables and load
    // the shared kernel GDT/IDT. APs remain kernel-only and do not use the BSP TSS.
    (void)enable_nx();
    asm volatile("mov %0, %%cr3" :: "r"(vmm_get_kernel_pml4()) : "memory");
    g_gdt->load_cpu_descriptors();
    g_idt->load_cpu_descriptor();
    enable_fpu_and_avx();
    g_lapic->initialize_current_cpu();

    SmpCpu& cpu = s_cpus[info->processor_id];
    cpu.lapic_id = info->lapic_id;
    cpu.processor_id = info->processor_id;
    __atomic_store_n(&cpu.active_root, vmm_get_kernel_pml4(), __ATOMIC_RELAXED);
    __atomic_store_n(&cpu.online, true, __ATOMIC_RELEASE);
    asm volatile("sti" ::: "memory");
    for (;;) asm volatile("hlt");
}

bool smp_initialize() {
    if (!g_mp_request.response) {
        boot_diagnostics_write("SMP: Limine MP response unavailable; staying on BSP\n");
        return false;
    }
    limine_mp_response* response = g_mp_request.response;
    if (!response->cpu_count || response->cpu_count > SMP_MAX_CPUS) return false;
    s_cpu_count = static_cast<uint32_t>(response->cpu_count);
    s_bootstrap_lapic_id = response->bsp_lapic_id;

    for (uint32_t i = 0; i < s_cpu_count; ++i) {
        limine_mp_info* info = response->cpus[i];
        if (!info || info->processor_id >= s_cpu_count) return false;
        SmpCpu& cpu = s_cpus[info->processor_id];
        cpu.processor_id = info->processor_id;
        cpu.lapic_id = info->lapic_id;
        cpu.bootstrap = info->lapic_id == response->bsp_lapic_id;
        cpu.online = cpu.bootstrap;
        cpu.command = SmpCommand::None;
        cpu.request_sequence = 0;
        cpu.acknowledged_sequence = 0;
        cpu.address_space = 0;
        cpu.active_root = cpu.bootstrap ? vmm_get_kernel_pml4() : 0;
        if (!cpu.bootstrap) info->goto_address = ap_entry;
    }

    if (s_cpu_count == 1) {
        s_initialized = true;
        boot_diagnostics_write("SMP: 1 CPU online\n");
        return true;
    }

    for (uint32_t i = 0; i < s_cpu_count; ++i) {
        SmpCpu& cpu = s_cpus[i];
        if (cpu.bootstrap) continue;
        const uint64_t deadline = read_tsc() + SMP_TIMEOUT_TICKS;
        while (!__atomic_load_n(&cpu.online, __ATOMIC_ACQUIRE) && read_tsc() < deadline)
            asm volatile("pause");
        if (!cpu.online) return false;
    }
    s_initialized = true;
    boot_diagnostics_write("SMP: all reported CPUs online\n");
    return true;
}

uint32_t smp_online_cpu_count() {
    uint32_t online = 0;
    for (uint32_t i = 0; i < s_cpu_count; ++i)
        if (__atomic_load_n(&s_cpus[i].online, __ATOMIC_ACQUIRE)) ++online;
    return online;
}

bool smp_is_bootstrap_processor() {
    return current_lapic_id() == s_bootstrap_lapic_id;
}

void smp_handle_ipi() {
    SmpCpu* cpu = cpu_by_lapic(current_lapic_id());
    if (!cpu || cpu->bootstrap) return;
    uint64_t sequence = __atomic_load_n(&cpu->request_sequence, __ATOMIC_ACQUIRE);
    if (sequence == __atomic_load_n(&cpu->acknowledged_sequence, __ATOMIC_RELAXED)) return;

    SmpCommand command = __atomic_load_n(&cpu->command, __ATOMIC_RELAXED);
    bool acknowledge = true;
    switch (command) {
        case SmpCommand::InvalidatePage: {
            uintptr_t address = __atomic_load_n(&cpu->virtual_address, __ATOMIC_RELAXED);
            asm volatile("invlpg (%0)" :: "r"(address) : "memory");
            break;
        }
        case SmpCommand::FlushNonGlobal: {
            uintptr_t cr3;
            asm volatile("mov %%cr3, %0" : "=r"(cr3));
            asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
            break;
        }
        case SmpCommand::SwitchAddressSpace: {
            uintptr_t root = __atomic_load_n(&cpu->address_space, __ATOMIC_RELAXED);
            asm volatile("mov %0, %%cr3" :: "r"(root) : "memory");
            __atomic_store_n(&cpu->active_root, root, __ATOMIC_RELEASE);
            break;
        }
        case SmpCommand::ReadVirtual: {
            uintptr_t address = __atomic_load_n(&cpu->virtual_address, __ATOMIC_RELAXED);
            uint64_t value = *reinterpret_cast<volatile uint64_t*>(address);
            __atomic_store_n(&cpu->read_value, value, __ATOMIC_RELAXED);
            break;
        }
        case SmpCommand::TestNoAck:
            acknowledge = false;
            break;
        case SmpCommand::None:
            break;
    }
    if (acknowledge)
        __atomic_store_n(&cpu->acknowledged_sequence, sequence, __ATOMIC_RELEASE);
}

static uint64_t lock_sender() {
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&s_sender_lock, __ATOMIC_ACQUIRE)) asm volatile("pause");
    return flags;
}
static void unlock_sender(uint64_t flags) {
    __atomic_clear(&s_sender_lock, __ATOMIC_RELEASE);
    if (flags & 0x200) asm volatile("sti" ::: "memory");
}

static bool issue_command(SmpCpu& cpu, SmpCommand command, uintptr_t address,
                          uintptr_t root, uint64_t* read_value = nullptr) {
    cpu.command = command;
    cpu.virtual_address = address;
    cpu.address_space = root;
    uint64_t sequence = ++s_next_sequence;
    __atomic_store_n(&cpu.request_sequence, sequence, __ATOMIC_RELEASE);
    if (!lapic_send_ipi(cpu.lapic_id, SMP_IPI_VECTOR)) return false;
    uint64_t deadline = read_tsc() + SMP_TIMEOUT_TICKS;
    while (__atomic_load_n(&cpu.acknowledged_sequence, __ATOMIC_ACQUIRE) != sequence) {
        if (read_tsc() >= deadline) return false;
        asm volatile("pause");
    }
    if (read_value) *read_value = __atomic_load_n(&cpu.read_value, __ATOMIC_RELAXED);
    return true;
}

bool smp_tlb_invalidate_page(uintptr_t virtual_address) {
    if (!s_initialized) return true;
    uint64_t flags = lock_sender();
    bool success = true;
    for (uint32_t i = 0; i < s_cpu_count && success; ++i) {
        SmpCpu& cpu = s_cpus[i];
        if (cpu.online && !cpu.bootstrap)
            success = issue_command(cpu, SmpCommand::InvalidatePage, virtual_address, 0);
    }
    unlock_sender(flags);
    return success;
}

bool smp_tlb_flush_non_global() {
    if (!s_initialized) return true;
    uint64_t flags = lock_sender();
    bool success = true;
    for (uint32_t i = 0; i < s_cpu_count && success; ++i) {
        SmpCpu& cpu = s_cpus[i];
        if (cpu.online && !cpu.bootstrap)
            success = issue_command(cpu, SmpCommand::FlushNonGlobal, 0, 0);
    }
    unlock_sender(flags);
    return success;
}

bool smp_address_space_active(uintptr_t root_phys) {
    for (uint32_t i = 0; i < s_cpu_count; ++i) {
        SmpCpu& cpu = s_cpus[i];
        if (cpu.online && !cpu.bootstrap &&
            __atomic_load_n(&cpu.active_root, __ATOMIC_ACQUIRE) == root_phys) return true;
    }
    return false;
}

bool smp_test_switch_address_space(uintptr_t root_phys) {
    if (!s_initialized) return false;
    uint64_t flags = lock_sender();
    bool success = true;
    for (uint32_t i = 0; i < s_cpu_count && success; ++i) {
        SmpCpu& cpu = s_cpus[i];
        if (cpu.online && !cpu.bootstrap)
            success = issue_command(cpu, SmpCommand::SwitchAddressSpace, 0, root_phys);
    }
    unlock_sender(flags);
    return success;
}

bool smp_test_read_virtual(uintptr_t virtual_address, uint64_t expected_value) {
    if (!s_initialized) return false;
    uint64_t flags = lock_sender();
    bool success = true;
    for (uint32_t i = 0; i < s_cpu_count && success; ++i) {
        SmpCpu& cpu = s_cpus[i];
        if (cpu.online && !cpu.bootstrap) {
            uint64_t value = 0;
            success = issue_command(cpu, SmpCommand::ReadVirtual, virtual_address, 0, &value) &&
                      value == expected_value;
        }
    }
    unlock_sender(flags);
    return success;
}


bool smp_test_timeout_recovery() {
    if (!s_initialized || smp_online_cpu_count() < 2) return false;
    uint64_t flags = lock_sender();
    SmpCpu* target = nullptr;
    for (uint32_t i = 0; i < s_cpu_count; ++i)
        if (s_cpus[i].online && !s_cpus[i].bootstrap) { target = &s_cpus[i]; break; }
    bool timed_out = target && !issue_command(*target, SmpCommand::TestNoAck, 0, 0);
    bool recovered = target && issue_command(*target, SmpCommand::FlushNonGlobal, 0, 0);
    unlock_sender(flags);
    return timed_out && recovered;
}
