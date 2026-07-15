#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guest/aarch64/runner.h"

#define CODE_PAGE UINT64_C(0x0000456789abc000)
#define SAMPLE_COUNT 10
#define TARGET_SAMPLE_NS UINT64_C(100000000)
#define BASELINE_MAD_WARNING_PERCENT 10.0
#define INITIAL_ITERATIONS UINT64_C(1024)
#define NOP_COUNT 60

#define INSTRUCTION_NOP UINT32_C(0xd503201f)
#define INSTRUCTION_ADD_X1_IMMEDIATE UINT32_C(0x91000421)
#define INSTRUCTION_ADD_X1_SHIFTED UINT32_C(0x8b020021)
#define INSTRUCTION_SUBS_X0 UINT32_C(0xf1000400)
#define INSTRUCTION_SVC UINT32_C(0xd4000001)

struct benchmark_memory {
    byte_t code[GUEST_MEMORY_PAGE_SIZE];
};

struct benchmark_environment {
    struct benchmark_memory memory;
    struct guest_address_space address_space;
    struct guest_tlb tlb;
    struct aarch64_runner runner;
};

struct benchmark_workload {
    const char *name;
    qword_t instructions_per_iteration;
    qword_t x1_increment_per_iteration;
    qword_t fast_per_iteration;
    qword_t fallback_per_iteration;
    size_t program_instruction_count;
    void (*write_program)(byte_t code[GUEST_MEMORY_PAGE_SIZE]);
};

enum benchmark_cache_state {
    BENCHMARK_CACHE_UNUSED,
    BENCHMARK_CACHE_COLD,
    BENCHMARK_CACHE_HOT,
};

struct benchmark_run {
    uint64_t elapsed_ns;
    struct cpu_state cpu;
    struct aarch64_step_result final_step;
};

struct benchmark_sample {
    uint64_t c_ns;
    uint64_t threaded_ns;
    double speedup;
};

static void fail(const char *workload, const char *message) {
    fprintf(stderr, "AArch64 后端基准失败（%s）：%s\n",
            workload, message);
    exit(EXIT_FAILURE);
}

static void require(bool condition, const char *workload,
        const char *message) {
    if (!condition)
        fail(workload, message);
}

static enum guest_memory_fault_kind resolve_code_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct benchmark_memory *memory = opaque;
    (void) access;
    if (page_base != CODE_PAGE)
        return GUEST_MEMORY_FAULT_UNMAPPED;
    *view = (struct guest_page_view) {
        .host_page = memory->code,
        .permissions = GUEST_MEMORY_EXECUTE,
    };
    return GUEST_MEMORY_FAULT_NONE;
}

static const struct guest_address_space_ops benchmark_address_space_ops = {
    .resolve_page = resolve_code_page,
};

static void put_instruction(byte_t *destination, dword_t instruction) {
    destination[0] = (byte_t) instruction;
    destination[1] = (byte_t) (instruction >> 8);
    destination[2] = (byte_t) (instruction >> 16);
    destination[3] = (byte_t) (instruction >> 24);
}

static dword_t encode_conditional_branch(int64_t displacement,
        byte_t condition) {
    dword_t immediate = (dword_t) (displacement / 4) &
            UINT32_C(0x7ffff);
    return UINT32_C(0x54000000) | immediate << 5 | condition;
}

static void write_fast_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ADD_X1_IMMEDIATE);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_mixed_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    put_instruction(code, INSTRUCTION_ADD_X1_SHIFTED);
    put_instruction(code + 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + 8, encode_conditional_branch(-8, 1));
    put_instruction(code + 12, INSTRUCTION_SVC);
}

static void write_nop_program(byte_t code[GUEST_MEMORY_PAGE_SIZE]) {
    for (unsigned index = 0; index < NOP_COUNT; index++)
        put_instruction(code + index * 4, INSTRUCTION_NOP);
    put_instruction(code + NOP_COUNT * 4, INSTRUCTION_SUBS_X0);
    put_instruction(code + (NOP_COUNT + 1) * 4,
            encode_conditional_branch(-(int64_t) ((NOP_COUNT + 1) * 4), 1));
    put_instruction(code + (NOP_COUNT + 2) * 4, INSTRUCTION_SVC);
}

static const struct benchmark_workload workloads[] = {
    {
        .name = "快速热点环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 1,
        .fast_per_iteration = 3,
        .fallback_per_iteration = 0,
        .program_instruction_count = 4,
        .write_program = write_fast_program,
    },
    {
        .name = "混合回落环",
        .instructions_per_iteration = 3,
        .x1_increment_per_iteration = 3,
        .fast_per_iteration = 2,
        .fallback_per_iteration = 1,
        .program_instruction_count = 4,
        .write_program = write_mixed_program,
    },
    {
        .name = "NOP 调度环",
        .instructions_per_iteration = NOP_COUNT + 2,
        .x1_increment_per_iteration = 0,
        .fast_per_iteration = NOP_COUNT + 2,
        .fallback_per_iteration = 0,
        .program_instruction_count = NOP_COUNT + 3,
        .write_program = write_nop_program,
    },
};

static qword_t scaled_count(const struct benchmark_workload *workload,
        qword_t iterations, qword_t per_iteration, qword_t tail) {
    if (per_iteration != 0 &&
            iterations > (UINT64_MAX - tail) / per_iteration)
        fail(workload->name, "迭代次数导致计数溢出");
    return iterations * per_iteration + tail;
}

static uint64_t monotonic_time_ns(const char *workload) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("AArch64 后端基准读取单调时钟失败");
        fail(workload, "无法读取单调时钟");
    }
    return (uint64_t) time.tv_sec * UINT64_C(1000000000) +
            (uint64_t) time.tv_nsec;
}

static void init_environment(struct benchmark_environment *environment,
        const struct benchmark_workload *workload,
        enum aarch64_backend backend) {
    *environment = (struct benchmark_environment) {0};
    workload->write_program(environment->memory.code);
    guest_address_space_init(&environment->address_space,
            &benchmark_address_space_ops, &environment->memory, 48);
    guest_tlb_init(&environment->tlb, &environment->address_space);
    require(aarch64_runner_init_backend(&environment->runner,
                    &environment->tlb, backend),
            workload->name, "请求的执行后端不可用");
}

static bool cpu_equal(const struct cpu_state *left,
        const struct cpu_state *right) {
    if (left->mmu != right->mmu || left->cycle != right->cycle ||
            memcmp(left->x, right->x, sizeof(left->x)) != 0 ||
            left->sp != right->sp || left->pc != right->pc ||
            left->nzcv != right->nzcv ||
            memcmp(left->v, right->v, sizeof(left->v)) != 0 ||
            left->fpcr != right->fpcr || left->fpsr != right->fpsr ||
            left->tpidr_el0 != right->tpidr_el0)
        return false;
    if (left->exclusive.address != right->exclusive.address ||
            left->exclusive.value_low != right->exclusive.value_low ||
            left->exclusive.value_high != right->exclusive.value_high ||
            left->exclusive.address_space != right->exclusive.address_space ||
            left->exclusive.mapping_epoch != right->exclusive.mapping_epoch ||
            left->exclusive.write_epoch != right->exclusive.write_epoch ||
            left->exclusive.size != right->exclusive.size ||
            left->exclusive.pair != right->exclusive.pair ||
            left->exclusive.valid != right->exclusive.valid)
        return false;
    return left->segfault_addr == right->segfault_addr &&
            left->segfault_was_write == right->segfault_was_write &&
            left->trapno == right->trapno &&
            left->single_step == right->single_step &&
            left->poked_ptr == right->poked_ptr &&
            left->_poked == right->_poked;
}

static bool step_equal(const struct aarch64_step_result *left,
        const struct aarch64_step_result *right) {
    return left->stop == right->stop &&
            left->instruction == right->instruction &&
            left->fault.address == right->fault.address &&
            left->fault.access == right->fault.access &&
            left->fault.kind == right->fault.kind;
}

static struct aarch64_threaded_stats stats_delta(
        const struct aarch64_threaded_stats *before,
        const struct aarch64_threaded_stats *after,
        const char *workload) {
    require(after->cache_hits >= before->cache_hits &&
                    after->cache_misses >= before->cache_misses &&
                    after->fast_dispatches >= before->fast_dispatches &&
                    after->c_fallbacks >= before->c_fallbacks,
            workload, "threaded 统计计数发生回退");
    return (struct aarch64_threaded_stats) {
        .cache_hits = after->cache_hits - before->cache_hits,
        .cache_misses = after->cache_misses - before->cache_misses,
        .fast_dispatches = after->fast_dispatches - before->fast_dispatches,
        .c_fallbacks = after->c_fallbacks - before->c_fallbacks,
    };
}

static void verify_stats(const struct benchmark_workload *workload,
        enum aarch64_backend backend, enum benchmark_cache_state cache_state,
        qword_t iterations, const struct aarch64_threaded_stats *delta) {
    if (backend == AARCH64_BACKEND_C) {
        require(delta->cache_hits == 0 && delta->cache_misses == 0 &&
                        delta->fast_dispatches == 0 &&
                        delta->c_fallbacks == 0,
                workload->name, "C oracle 意外修改 threaded 统计");
        return;
    }

    qword_t total = scaled_count(workload, iterations,
            workload->instructions_per_iteration, 1);
    qword_t fast = scaled_count(workload, iterations,
            workload->fast_per_iteration, 1);
    qword_t fallbacks = scaled_count(workload, iterations,
            workload->fallback_per_iteration, 0);
    require(fast + fallbacks == total,
            workload->name, "工作负载的分派计数定义不一致");
    require(delta->fast_dispatches == fast &&
                    delta->c_fallbacks == fallbacks,
            workload->name, "threaded 快速或回落分派计数不符");

    if (cache_state == BENCHMARK_CACHE_COLD) {
        require(iterations == 1,
                workload->name, "冷缓存预热必须只运行一轮");
        require(delta->cache_hits == 0 &&
                        delta->cache_misses ==
                                workload->program_instruction_count,
                workload->name, "冷缓存没有恰好填满工作集");
    } else if (cache_state == BENCHMARK_CACHE_HOT) {
        require(delta->cache_hits == total && delta->cache_misses == 0,
                workload->name, "计时区间出现 threaded 缓存未命中");
    } else {
        fail(workload->name, "threaded 运行缺少缓存状态约束");
    }
}

static void verify_run(const struct benchmark_workload *workload,
        qword_t iterations, const struct benchmark_run *run) {
    qword_t expected_cycles = scaled_count(workload, iterations,
            workload->instructions_per_iteration, 1);
    qword_t expected_x1 = scaled_count(workload, iterations,
            workload->x1_increment_per_iteration, 7);
    require(run->final_step.stop == AARCH64_STEP_SYSCALL &&
                    run->final_step.instruction == INSTRUCTION_SVC,
            workload->name, "程序没有停在预期的 SVC 边界");
    require(run->cpu.cycle == expected_cycles && run->cpu.x[0] == 0 &&
                    run->cpu.x[1] == expected_x1 && run->cpu.x[2] == 3,
            workload->name, "循环次数或通用寄存器结果不符");
    require(run->cpu.pc == CODE_PAGE +
                    workload->program_instruction_count * 4 &&
                    run->cpu.nzcv == UINT32_C(0x60000000),
            workload->name, "PC 或 NZCV 最终状态不符");
}

static struct benchmark_run run_workload(
        struct benchmark_environment *environment,
        const struct benchmark_workload *workload, qword_t iterations,
        enum benchmark_cache_state cache_state) {
    struct cpu_state cpu = {
        .x[0] = iterations,
        .x[1] = 7,
        .x[2] = 3,
        .pc = CODE_PAGE,
        .nzcv = UINT32_C(0x90000000),
    };
    enum aarch64_backend backend =
            aarch64_runner_backend(&environment->runner);
    struct aarch64_threaded_stats before =
            *aarch64_runner_threaded_stats(&environment->runner);
    qword_t step_count = scaled_count(workload, iterations,
            workload->instructions_per_iteration, 1);
    uint64_t started = monotonic_time_ns(workload->name);
    struct aarch64_step_result step;
    for (qword_t index = 0; index < step_count; index++)
        step = aarch64_run_one(&environment->runner, &cpu);
    uint64_t finished = monotonic_time_ns(workload->name);
    struct aarch64_threaded_stats after =
            *aarch64_runner_threaded_stats(&environment->runner);
    struct aarch64_threaded_stats delta = stats_delta(
            &before, &after, workload->name);
    verify_stats(workload, backend, cache_state, iterations, &delta);

    struct benchmark_run run = {
        .elapsed_ns = finished - started,
        .cpu = cpu,
        .final_step = step,
    };
    verify_run(workload, iterations, &run);
    return run;
}

static void verify_pair(const struct benchmark_workload *workload,
        const struct benchmark_run *c_run,
        const struct benchmark_run *threaded_run) {
    require(cpu_equal(&c_run->cpu, &threaded_run->cpu),
            workload->name, "C oracle 与 threaded 最终 CPU 状态不同");
    require(step_equal(&c_run->final_step, &threaded_run->final_step),
            workload->name, "C oracle 与 threaded 最终停止结果不同");
}

static qword_t calibrate_iterations(
        struct benchmark_environment *c_environment,
        struct benchmark_environment *threaded_environment,
        const struct benchmark_workload *workload) {
    qword_t iterations = INITIAL_ITERATIONS;
    while (true) {
        struct benchmark_run c_run = run_workload(c_environment,
                workload, iterations, BENCHMARK_CACHE_UNUSED);
        struct benchmark_run threaded_run = run_workload(
                threaded_environment, workload, iterations,
                BENCHMARK_CACHE_HOT);
        verify_pair(workload, &c_run, &threaded_run);
        if (c_run.elapsed_ns >= TARGET_SAMPLE_NS &&
                threaded_run.elapsed_ns >= TARGET_SAMPLE_NS)
            return iterations;
        if (iterations > UINT64_MAX / 2)
            fail(workload->name, "无法在计数范围内达到目标采样时长");
        iterations *= 2;
    }
}

static int compare_uint64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *) left;
    uint64_t b = *(const uint64_t *) right;
    return (a > b) - (a < b);
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *) left;
    double b = *(const double *) right;
    return (a > b) - (a < b);
}

static uint64_t median_uint64(const uint64_t values[SAMPLE_COUNT]) {
    uint64_t sorted[SAMPLE_COUNT];
    memcpy(sorted, values, sizeof(sorted));
    qsort(sorted, SAMPLE_COUNT, sizeof(sorted[0]), compare_uint64);
    uint64_t lower = sorted[SAMPLE_COUNT / 2 - 1];
    uint64_t upper = sorted[SAMPLE_COUNT / 2];
    return lower + (upper - lower) / 2;
}

static double median_double(const double values[SAMPLE_COUNT]) {
    double sorted[SAMPLE_COUNT];
    memcpy(sorted, values, sizeof(sorted));
    qsort(sorted, SAMPLE_COUNT, sizeof(sorted[0]), compare_double);
    double lower = sorted[SAMPLE_COUNT / 2 - 1];
    double upper = sorted[SAMPLE_COUNT / 2];
    return lower + (upper - lower) / 2;
}

static double duration_mad_percent(
        const uint64_t values[SAMPLE_COUNT], uint64_t median) {
    uint64_t deviations[SAMPLE_COUNT];
    for (unsigned index = 0; index < SAMPLE_COUNT; index++) {
        deviations[index] = values[index] >= median ?
                values[index] - median : median - values[index];
    }
    return median == 0 ? 0 :
            (double) median_uint64(deviations) * 100 / (double) median;
}

static double ratio_mad_percent(
        const double values[SAMPLE_COUNT], double median) {
    double deviations[SAMPLE_COUNT];
    for (unsigned index = 0; index < SAMPLE_COUNT; index++) {
        double difference = values[index] - median;
        deviations[index] = difference >= 0 ? difference : -difference;
    }
    return median == 0 ? 0 :
            median_double(deviations) * 100 / median;
}

static void benchmark_workload(const struct benchmark_workload *workload) {
    struct benchmark_environment c_environment;
    struct benchmark_environment threaded_environment;
    init_environment(&c_environment, workload, AARCH64_BACKEND_C);
    init_environment(&threaded_environment, workload,
            AARCH64_BACKEND_THREADED);

    // 一轮覆盖完整工作集，计时区间只衡量稳定命中路径。
    struct benchmark_run c_warm = run_workload(&c_environment,
            workload, 1, BENCHMARK_CACHE_UNUSED);
    struct benchmark_run threaded_warm = run_workload(
            &threaded_environment, workload, 1, BENCHMARK_CACHE_COLD);
    verify_pair(workload, &c_warm, &threaded_warm);

    qword_t iterations = calibrate_iterations(&c_environment,
            &threaded_environment, workload);
    struct benchmark_sample samples[SAMPLE_COUNT];
    for (unsigned index = 0; index < SAMPLE_COUNT; index++) {
        struct benchmark_run c_run;
        struct benchmark_run threaded_run;
        if ((index & 1) == 0) {
            c_run = run_workload(&c_environment, workload,
                    iterations, BENCHMARK_CACHE_UNUSED);
            threaded_run = run_workload(&threaded_environment,
                    workload, iterations, BENCHMARK_CACHE_HOT);
        } else {
            threaded_run = run_workload(&threaded_environment,
                    workload, iterations, BENCHMARK_CACHE_HOT);
            c_run = run_workload(&c_environment, workload,
                    iterations, BENCHMARK_CACHE_UNUSED);
        }
        verify_pair(workload, &c_run, &threaded_run);
        samples[index] = (struct benchmark_sample) {
            .c_ns = c_run.elapsed_ns,
            .threaded_ns = threaded_run.elapsed_ns,
            .speedup = (double) c_run.elapsed_ns /
                    (double) threaded_run.elapsed_ns,
        };
    }

    uint64_t c_values[SAMPLE_COUNT];
    uint64_t threaded_values[SAMPLE_COUNT];
    double ratios[SAMPLE_COUNT];
    for (unsigned index = 0; index < SAMPLE_COUNT; index++) {
        c_values[index] = samples[index].c_ns;
        threaded_values[index] = samples[index].threaded_ns;
        ratios[index] = samples[index].speedup;
    }
    uint64_t c_median = median_uint64(c_values);
    uint64_t threaded_median = median_uint64(threaded_values);
    double ratio_median = median_double(ratios);
    double c_mad = duration_mad_percent(c_values, c_median);
    double threaded_mad = duration_mad_percent(
            threaded_values, threaded_median);
    double ratio_mad = ratio_mad_percent(ratios, ratio_median);
    qword_t instruction_count = scaled_count(workload, iterations,
            workload->instructions_per_iteration, 1);

    printf("\n工作负载：%s\n", workload->name);
    printf("  每组迭代：%" PRIu64 "，guest 指令：%" PRIu64 "\n",
            iterations, instruction_count);
    printf("  C oracle：%.3f ns/guest 指令（中位数，MAD %.2f%%）\n",
            (double) c_median / (double) instruction_count,
            c_mad);
    printf("  threaded：%.3f ns/guest 指令（中位数，MAD %.2f%%）\n",
            (double) threaded_median / (double) instruction_count,
            threaded_mad);
    printf("  配对加速比：%.3fx（中位数，MAD %.2f%%）\n",
            ratio_median, ratio_mad);
    if (c_mad > BASELINE_MAD_WARNING_PERCENT ||
            threaded_mad > BASELINE_MAD_WARNING_PERCENT ||
            ratio_mad > BASELINE_MAD_WARNING_PERCENT) {
        printf("  警告：本轮离散度超过 %.0f%%，不宜作为性能回归基线。\n",
                BASELINE_MAD_WARNING_PERCENT);
    }
}

int main(void) {
    printf("AArch64 C/threaded 原生微基准\n");
    printf("每项自适应到至少 100 ms，共 %d 组交替次序配对样本。\n",
            SAMPLE_COUNT);
    for (unsigned index = 0; index <
            sizeof(workloads) / sizeof(workloads[0]); index++)
        benchmark_workload(&workloads[index]);
    return 0;
}
