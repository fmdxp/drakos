#include "kernel/address_space_runtime_test.hpp"

#include "kernel/boot_diagnostics.hpp"
#include "kernel/thread.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"
#include "cpu.hpp"

extern "C" const uint8_t process_exit_worker_start[];
extern "C" const uint8_t process_exit_worker_end[];
extern "C" const uint8_t process_exit_wait_37[];
extern "C" const uint8_t process_exit_wait_41[];
extern "C" const uint8_t process_exit_zero[];
extern "C" const uint8_t process_exit_test_end[];

namespace {
constexpr uintptr_t kTestCodeAddress = 0x00400000;
constexpr uintptr_t kTestDataAddress = 0x00401000;
constexpr uint64_t kExitDataFlags = VMM_PRESENT | VMM_USER | VMM_WRITE;

bool map_test_pages(Process* process, uintptr_t code_frame, uintptr_t data_frame) {
    uint64_t data_flags = kExitDataFlags;
    if (nx_enabled()) data_flags |= VMM_NX;
    return vmm_map_page(process->page_table_phys, kTestCodeAddress, code_frame,
                        VMM_PRESENT | VMM_USER) == VmmMapResult::Success &&
           vmm_map_page(process->page_table_phys, kTestDataAddress, data_frame,
                        data_flags) == VmmMapResult::Success;
}

void clear_page(uintptr_t frame) {
    auto* bytes = reinterpret_cast<uint8_t*>(frame + pmm_hhdm_offset());
    for (size_t i = 0; i < PAGE_SIZE; ++i) bytes[i] = 0;
}

bool wait_for_exit(Process* process, bool* saw_terminating = nullptr) {
    if (saw_terminating) *saw_terminating = false;
    for (size_t attempt = 0; attempt < 10000 &&
         process->state() != ProcessState::Exited; ++attempt) {
        if (saw_terminating && process->state() == ProcessState::Terminating)
            *saw_terminating = true;
        scheduler_yield();
    }
    return process->state() == ProcessState::Exited;
}

void free_frames(uintptr_t code, uintptr_t zero_data, uintptr_t victim_data,
                 uintptr_t unrelated_data) {
    if (code) pmm_free_page(code);
    if (zero_data) pmm_free_page(zero_data);
    if (victim_data) pmm_free_page(victim_data);
    if (unrelated_data) pmm_free_page(unrelated_data);
}
}

bool run_process_exit_runtime_test() {
    uintptr_t code_frame = pmm_alloc_page();
    uintptr_t zero_data_frame = pmm_alloc_page();
    uintptr_t victim_data_frame = pmm_alloc_page();
    uintptr_t unrelated_data_frame = pmm_alloc_page();
    if (!code_frame || !zero_data_frame || !victim_data_frame || !unrelated_data_frame) {
        free_frames(code_frame, zero_data_frame, victim_data_frame, unrelated_data_frame);
        boot_diagnostics_write("process exit semantics: FAIL (page allocation)\n");
        return false;
    }

    auto* code = reinterpret_cast<uint8_t*>(code_frame + pmm_hhdm_offset());
    for (size_t i = 0; i < PAGE_SIZE; ++i) code[i] = 0xCC;
    size_t code_size = static_cast<size_t>(process_exit_test_end - process_exit_worker_start);
    for (size_t i = 0; i < code_size; ++i) code[i] = process_exit_worker_start[i];
    clear_page(zero_data_frame);
    clear_page(victim_data_frame);
    clear_page(unrelated_data_frame);

    Process* zero_process = nullptr;
    bool zero_ready = Process::create_user(&zero_process) == ProcessResult::Success &&
        zero_process && map_test_pages(zero_process, code_frame, zero_data_frame);
    Thread* zero_thread = zero_ready ? new Thread(zero_process,
        reinterpret_cast<void(*)()>(kTestCodeAddress +
            static_cast<uintptr_t>(process_exit_zero - process_exit_worker_start)),
        true, "exit-status-zero") : nullptr;
    bool zero_published = zero_thread && zero_thread->state != THREAD_DEAD &&
        scheduler_add_thread(zero_thread) == SchedulerPublishResult::Success;
    if (!zero_published) {
        if (zero_thread) delete zero_thread;
        if (zero_process) (void)zero_process->release();
        free_frames(code_frame, zero_data_frame, victim_data_frame, unrelated_data_frame);
        boot_diagnostics_write("process exit semantics: FAIL (zero-status setup)\n");
        return false;
    }

    bool zero_exited = wait_for_exit(zero_process);
    bool zero_status_ok = zero_exited && zero_process->exit_status() == 0 &&
        zero_process->has_explicit_exit_status() &&
        scheduler_count_process_threads(zero_process) == 0 &&
        zero_process->reference_count() == 1;
    bool zero_released = zero_process->release() == ProcessResult::Success;
    boot_diagnostics_write(zero_status_ok && zero_released
        ? "process exit status zero: PASS\n" : "process exit status zero: FAIL\n");
    if (!zero_exited || !zero_released) return false;

    Process* victim = nullptr;
    Process* unrelated = nullptr;
    bool processes_ready = Process::create_user(&victim) == ProcessResult::Success && victim &&
        Process::create_user(&unrelated) == ProcessResult::Success && unrelated;
    bool mappings_ready = processes_ready &&
        map_test_pages(victim, code_frame, victim_data_frame) &&
        map_test_pages(unrelated, code_frame, unrelated_data_frame);
    if (!mappings_ready) {
        if (victim) (void)victim->release();
        if (unrelated) (void)unrelated->release();
        free_frames(code_frame, zero_data_frame, victim_data_frame, unrelated_data_frame);
        boot_diagnostics_write("process exit semantics: FAIL (multi-process setup)\n");
        return false;
    }
    boot_diagnostics_write("process exit test: mappings ready\n");

    auto entry = [](const uint8_t* symbol) {
        return reinterpret_cast<void(*)()>(kTestCodeAddress +
            static_cast<uintptr_t>(symbol - process_exit_worker_start));
    };
    Thread* victim_worker = new Thread(victim, entry(process_exit_worker_start), true,
                                        "exit-victim-worker");
    Thread* exit37 = new Thread(victim, entry(process_exit_wait_37), true, "exit-victim-37");
    Thread* exit41 = new Thread(victim, entry(process_exit_wait_41), true, "exit-victim-41");
    Thread* unrelated_thread = new Thread(unrelated, entry(process_exit_worker_start), true,
                                          "exit-unrelated-worker");
    bool threads_ready = victim_worker && exit37 && exit41 && unrelated_thread &&
        victim_worker->state != THREAD_DEAD && exit37->state != THREAD_DEAD &&
        exit41->state != THREAD_DEAD && unrelated_thread->state != THREAD_DEAD;
    if (!threads_ready) {
        delete victim_worker; delete exit37; delete exit41; delete unrelated_thread;
        (void)victim->release(); (void)unrelated->release();
        free_frames(code_frame, zero_data_frame, victim_data_frame, unrelated_data_frame);
        boot_diagnostics_write("process exit semantics: FAIL (thread setup)\n");
        return false;
    }

    bool worker_published = scheduler_add_thread(victim_worker) == SchedulerPublishResult::Success;
    bool exit37_published = worker_published &&
        scheduler_add_thread(exit37) == SchedulerPublishResult::Success;
    bool exit41_published = exit37_published &&
        scheduler_add_thread(exit41) == SchedulerPublishResult::Success;
    bool unrelated_published = exit41_published &&
        scheduler_add_thread(unrelated_thread) == SchedulerPublishResult::Success;
    if (!unrelated_published) {
        if (worker_published) scheduler_terminate_thread(victim_worker); else delete victim_worker;
        if (exit37_published) scheduler_terminate_thread(exit37); else delete exit37;
        if (exit41_published) scheduler_terminate_thread(exit41); else delete exit41;
        while (scheduler_count_process_threads(victim)) scheduler_yield();
        delete unrelated_thread;
        (void)victim->release(); (void)unrelated->release();
        free_frames(code_frame, zero_data_frame, victim_data_frame, unrelated_data_frame);
        boot_diagnostics_write("process exit semantics: FAIL (thread publication)\n");
        return false;
    }
    boot_diagnostics_write("process exit test: threads published\n");

    auto* victim_data = reinterpret_cast<volatile uint64_t*>(
        victim_data_frame + pmm_hhdm_offset());
    auto* unrelated_data = reinterpret_cast<volatile uint64_t*>(
        unrelated_data_frame + pmm_hhdm_offset());
    bool concurrent_execution = false;
    for (size_t attempt = 0; attempt < 3000; ++attempt) {
        concurrent_execution = scheduler_count_running_process_cpus(victim) >= 2 &&
            scheduler_count_running_process_cpus(unrelated) >= 1 &&
            __atomic_load_n(&victim_data[0], __ATOMIC_RELAXED) != 0 &&
            __atomic_load_n(&unrelated_data[0], __ATOMIC_RELAXED) != 0;
        if (concurrent_execution) break;
        scheduler_yield();
    }
    boot_diagnostics_write(concurrent_execution
        ? "process exit test: concurrent workers observed\n"
        : "process exit test: concurrent worker timeout\n");

    // Both exit callers race after the shared release flag. The first status
    // committed under the scheduler lock must remain stable.
    __atomic_store_n(&victim_data[1], 1, __ATOMIC_RELEASE);
    bool saw_terminating = false;
    bool victim_exited = wait_for_exit(victim, &saw_terminating);
    boot_diagnostics_write(victim_exited ? "process exit test: victim cleanup complete\n"
                                         : "process exit test: victim cleanup timeout\n");
    int32_t status = victim->exit_status();
    uint64_t victim_count_after_exit = __atomic_load_n(&victim_data[0], __ATOMIC_RELAXED);
    uint64_t unrelated_count_before = __atomic_load_n(&unrelated_data[0], __ATOMIC_RELAXED);
    for (size_t attempt = 0; attempt < 100; ++attempt) scheduler_yield();
    uint64_t unrelated_count_after = __atomic_load_n(&unrelated_data[0], __ATOMIC_RELAXED);

    bool all_victim_threads_stopped = victim_exited &&
        __atomic_load_n(&victim_data[0], __ATOMIC_RELAXED) == victim_count_after_exit &&
        scheduler_count_process_threads(victim) == 0 &&
        scheduler_count_running_process_cpus(victim) == 0;
    bool status_preserved = victim->has_explicit_exit_status() &&
        (status == 37 || status == 41) && victim->reference_count() == 1;
    bool unrelated_continued = unrelated_count_after > unrelated_count_before &&
        unrelated->state() != ProcessState::Exited &&
        scheduler_count_process_threads(unrelated) == 1;
    boot_diagnostics_write(concurrent_execution && saw_terminating &&
        all_victim_threads_stopped && status_preserved
        ? "process-wide concurrent SMP exit: PASS\n" :
          "process-wide concurrent SMP exit: FAIL\n");
    boot_diagnostics_write(unrelated_continued
        ? "unrelated process continued after exit: PASS\n" :
          "unrelated process continued after exit: FAIL\n");

    scheduler_terminate_thread(unrelated_thread);
    bool unrelated_exited = wait_for_exit(unrelated);
    for (size_t attempt = 0; attempt < 1000 && unrelated->reference_count() > 1; ++attempt)
        scheduler_yield();
    bool default_status = unrelated_exited && unrelated->exit_status() == 0 &&
        !unrelated->has_explicit_exit_status() && unrelated->reference_count() == 1;
    bool victim_released = victim->release() == ProcessResult::Success;
    bool unrelated_released = unrelated->release() == ProcessResult::Success;
    free_frames(code_frame, zero_data_frame, victim_data_frame, unrelated_data_frame);
    bool passed = zero_status_ok && victim_exited && all_victim_threads_stopped &&
        saw_terminating && status_preserved && unrelated_continued && default_status && victim_released &&
        unrelated_released;
    boot_diagnostics_write(default_status
        ? "implicit process exit status defaults to zero: PASS\n" :
          "implicit process exit status defaults to zero: FAIL\n");
    boot_diagnostics_write(passed ? "process exit semantics: PASS\n" :
                                    "process exit semantics: FAIL\n");
    return passed;
}
