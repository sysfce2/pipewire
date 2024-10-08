/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "test-helper.h"
#include "fmt-ops.h"

static uint32_t cpu_flags;

typedef void (*convert_func_t) (struct convert *conv, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples);

struct stats {
	uint32_t n_samples;
	uint32_t n_channels;
	uint64_t perf;
	const char *name;
	const char *impl;
};

#define MAX_SAMPLES	4096
#define MAX_CHANNELS	11

#define MAX_COUNT 100

static uint8_t samp_in[MAX_SAMPLES * MAX_CHANNELS * 4];
static uint8_t samp_out[MAX_SAMPLES * MAX_CHANNELS * 4];

static const int sample_sizes[] = { 0, 1, 128, 513, 4096 };
static const int channel_counts[] = { 1, 2, 4, 6, 8, 11 };

#define MAX_RESULTS	SPA_N_ELEMENTS(sample_sizes) * SPA_N_ELEMENTS(channel_counts) * 70

static uint32_t n_results = 0;
static struct stats results[MAX_RESULTS];

static void run_test1(const char *name, const char *impl, bool in_packed, bool out_packed,
		convert_func_t func, int n_channels, int n_samples)
{
	int i, j;
	const void *ip[n_channels];
	void *op[n_channels];
	struct timespec ts;
	uint64_t count, t1, t2;
	struct convert conv;

	conv.n_channels = n_channels;

	for (j = 0; j < n_channels; j++) {
		ip[j] = &samp_in[j * n_samples * 4];
		op[j] = &samp_out[j * n_samples * 4];
	}

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t1 = SPA_TIMESPEC_TO_NSEC(&ts);

	count = 0;
	for (i = 0; i < MAX_COUNT; i++) {
		func(&conv, op, ip, n_samples);
		count++;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t2 = SPA_TIMESPEC_TO_NSEC(&ts);

	spa_assert(n_results < MAX_RESULTS);

	results[n_results++] = (struct stats) {
		.n_samples = n_samples,
		.n_channels = n_channels,
		.perf = count * (uint64_t)SPA_NSEC_PER_SEC / (t2 - t1),
		.name = name,
		.impl = impl
	};
}

static void run_testc(const char *name, const char *impl, bool in_packed, bool out_packed, convert_func_t func,
		int channel_count)
{
	SPA_FOR_EACH_ELEMENT_VAR(sample_sizes, s) {
		run_test1(name, impl, in_packed, out_packed, func, channel_count,
				(*s + (channel_count -1)) / channel_count);
	}
}

static void run_test(const char *name, const char *impl, bool in_packed, bool out_packed, convert_func_t func)
{
	SPA_FOR_EACH_ELEMENT_VAR(sample_sizes, s) {
		SPA_FOR_EACH_ELEMENT_VAR(channel_counts, c) {
			run_test1(name, impl, in_packed, out_packed, func, *c, (*s + (*c -1)) / *c);
		}
	}
}

static void test_f32_u8(void)
{
	run_test("test_f32_u8", "c", true, true, conv_f32_to_u8_c);
	run_test("test_f32d_u8", "c", false, true, conv_f32d_to_u8_c);
	run_test("test_f32_u8d", "c", true, false, conv_f32_to_u8d_c);
	run_test("test_f32d_u8d", "c", false, false, conv_f32d_to_u8d_c);
}

static void test_u8_f32(void)
{
	run_test("test_u8_f32", "c", true, true, conv_u8_to_f32_c);
	run_test("test_u8d_f32", "c", false, true, conv_u8d_to_f32_c);
	run_test("test_u8_f32d", "c", true, false, conv_u8_to_f32d_c);
	run_test("test_u8d_f32d", "c", false, false, conv_u8d_to_f32d_c);
}

static void test_f32_s16(void)
{
	run_test("test_f32_s16", "c", true, true, conv_f32_to_s16_c);
	run_test("test_f32d_s16", "c", false, true, conv_f32d_to_s16_c);
#if defined (HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_f32d_s16", "sse2", false, true, conv_f32d_to_s16_sse2);
		run_testc("test_f32d_s16_2", "sse2", false, true, conv_f32d_to_s16_2_sse2, 2);
	}
#endif
#if defined (HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_f32d_s16", "avx2", false, true, conv_f32d_to_s16_avx2);
		run_testc("test_f32d_s16_2", "avx2", false, true, conv_f32d_to_s16_2_avx2, 2);
		run_testc("test_f32d_s16_4", "avx2", false, true, conv_f32d_to_s16_4_avx2, 4);
	}
#endif
#if defined (HAVE_RVV)
	if (cpu_flags & SPA_CPU_FLAG_RISCV_V) {
		run_test("test_f32_s16", "rvv", true, true, conv_f32_to_s16_rvv);
		run_test("test_f32d_s16d", "rvv", false, false, conv_f32d_to_s16d_rvv);
		run_test("test_f32d_s16", "rvv", false, true, conv_f32d_to_s16_rvv);
	}
#endif
	run_test("test_f32_s16d", "c", true, false, conv_f32_to_s16d_c);
	run_test("test_f32d_s16d", "c", false, false, conv_f32d_to_s16d_c);
}

static void test_s16_f32(void)
{
	run_test("test_s16_f32", "c", true, true, conv_s16_to_f32_c);
	run_test("test_s16d_f32", "c", false, true, conv_s16d_to_f32_c);
	run_test("test_s16_f32d", "c", true, false, conv_s16_to_f32d_c);
#if defined (HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_s16_f32d", "sse2", true, false, conv_s16_to_f32d_sse2);
		run_testc("test_s16_f32d_2", "sse2", true, false, conv_s16_to_f32d_2_sse2, 2);
	}
#endif
#if defined (HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_s16_f32d", "avx2", true, false, conv_s16_to_f32d_avx2);
		run_testc("test_s16_f32d_2", "avx2", true, false, conv_s16_to_f32d_2_avx2, 2);
	}
#endif
#if defined (HAVE_RVV)
	if (cpu_flags & SPA_CPU_FLAG_RISCV_V) {
		run_test("test_s16_f32d", "rvv", true, false, conv_s16_to_f32d_rvv);
	}
#endif
	run_test("test_s16d_f32d", "c", false, false, conv_s16d_to_f32d_c);
}

static void test_f32_s32(void)
{
	run_test("test_f32_s32", "c", true, true, conv_f32_to_s32_c);
	run_test("test_f32d_s32", "c", false, true, conv_f32d_to_s32_c);
#if defined (HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_f32d_s32", "sse2", false, true, conv_f32d_to_s32_sse2);
	}
#endif
#if defined (HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_f32d_s32", "avx2", false, true, conv_f32d_to_s32_avx2);
	}
#endif
#if defined (HAVE_RVV)
	if (cpu_flags & SPA_CPU_FLAG_RISCV_V) {
		run_test("test_f32d_s32", "rvv", false, true, conv_f32d_to_s32_rvv);
	}
#endif
	run_test("test_f32_s32d", "c", true, false, conv_f32_to_s32d_c);
	run_test("test_f32d_s32d", "c", false, false, conv_f32d_to_s32d_c);
}

static void test_s32_f32(void)
{
	run_test("test_s32_f32", "c", true, true, conv_s32_to_f32_c);
	run_test("test_s32d_f32", "c", false, true, conv_s32d_to_f32_c);
#if defined (HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_s32_f32d", "sse2", true, false, conv_s32_to_f32d_sse2);
	}
#endif
#if defined (HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_s32_f32d", "avx2", true, false, conv_s32_to_f32d_avx2);
	}
#endif
#if defined (HAVE_RVV)
	if (cpu_flags & SPA_CPU_FLAG_RISCV_V) {
		run_test("test_s32_f32d", "rvv", true, false, conv_s32_to_f32d_rvv);
	}
#endif
	run_test("test_s32_f32d", "c", true, false, conv_s32_to_f32d_c);
	run_test("test_s32d_f32d", "c", false, false, conv_s32d_to_f32d_c);
}

static void test_f32_s24(void)
{
	run_test("test_f32_s24", "c", true, true, conv_f32_to_s24_c);
	run_test("test_f32d_s24", "c", false, true, conv_f32d_to_s24_c);
	run_test("test_f32_s24d", "c", true, false, conv_f32_to_s24d_c);
	run_test("test_f32d_s24d", "c", false, false, conv_f32d_to_s24d_c);
}

static void test_s24_f32(void)
{
	run_test("test_s24_f32", "c", true, true, conv_s24_to_f32_c);
	run_test("test_s24d_f32", "c", false, true, conv_s24d_to_f32_c);
	run_test("test_s24_f32d", "c", true, false, conv_s24_to_f32d_c);
#if defined (HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_s24_f32d", "sse2", true, false, conv_s24_to_f32d_sse2);
	}
#endif
#if defined (HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_s24_f32d", "avx2", true, false, conv_s24_to_f32d_avx2);
	}
#endif
#if defined (HAVE_SSSE3)
	if (cpu_flags & SPA_CPU_FLAG_SSSE3) {
		run_test("test_s24_f32d", "ssse3", true, false, conv_s24_to_f32d_ssse3);
	}
#endif
#if defined (HAVE_SSE41)
	if (cpu_flags & SPA_CPU_FLAG_SSE41) {
		run_test("test_s24_f32d", "sse41", true, false, conv_s24_to_f32d_sse41);
	}
#endif
	run_test("test_s24d_f32d", "c", false, false, conv_s24d_to_f32d_c);
}

static void test_f32_s24_32(void)
{
	run_test("test_f32_s24_32", "c", true, true, conv_f32_to_s24_32_c);
	run_test("test_f32d_s24_32", "c", false, true, conv_f32d_to_s24_32_c);
	run_test("test_f32_s24_32d", "c", true, false, conv_f32_to_s24_32d_c);
	run_test("test_f32d_s24_32d", "c", false, false, conv_f32d_to_s24_32d_c);
}

static void test_s24_32_f32(void)
{
	run_test("test_s24_32_f32", "c", true, true, conv_s24_32_to_f32_c);
	run_test("test_s24_32d_f32", "c", false, true, conv_s24_32d_to_f32_c);
	run_test("test_s24_32_f32d", "c", true, false, conv_s24_32_to_f32d_c);
	run_test("test_s24_32d_f32d", "c", false, false, conv_s24_32d_to_f32d_c);
}

static void test_interleave(void)
{
	run_test("test_8d_to_8", "c", false, true, conv_8d_to_8_c);
	run_test("test_16d_to_16", "c", false, true, conv_16d_to_16_c);
	run_test("test_24d_to_24", "c", false, true, conv_24d_to_24_c);
	run_test("test_32d_to_32", "c", false, true, conv_32d_to_32_c);
}

static void test_deinterleave(void)
{
	run_test("test_8_to_8d", "c", true, false, conv_8_to_8d_c);
	run_test("test_16_to_16d", "c", true, false, conv_16_to_16d_c);
	run_test("test_24_to_24d", "c", true, false, conv_24_to_24d_c);
	run_test("test_32_to_32d", "c", true, false, conv_32_to_32d_c);
}

static int compare_func(const void *_a, const void *_b)
{
	const struct stats *a = _a, *b = _b;
	int diff;
	if ((diff = strcmp(a->name, b->name)) != 0) return diff;
	if ((diff = a->n_samples - b->n_samples) != 0) return diff;
	if ((diff = a->n_channels - b->n_channels) != 0) return diff;
	if ((diff = b->perf - a->perf) != 0) return diff;
	return 0;
}

int main(int argc, char *argv[])
{
	uint32_t i;

	cpu_flags = get_cpu_flags();
	printf("got get CPU flags %d\n", cpu_flags);

	test_f32_u8();
	test_u8_f32();
	test_f32_s16();
	test_s16_f32();
	test_f32_s32();
	test_s32_f32();
	test_f32_s24();
	test_s24_f32();
	test_f32_s24_32();
	test_s24_32_f32();
	test_interleave();
	test_deinterleave();

	qsort(results, n_results, sizeof(struct stats), compare_func);

	for (i = 0; i < n_results; i++) {
		struct stats *s = &results[i];
		fprintf(stderr, "%-12."PRIu64" \t%-32.32s %s \t samples %d, channels %d\n",
				s->perf, s->name, s->impl, s->n_samples, s->n_channels);
	}
	return 0;
}
