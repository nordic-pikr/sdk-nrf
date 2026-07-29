/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "perf_common.h"

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>

const struct device *const can_perf_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/*
 * API test bitrates (125k, 250k, 1M) plus 500 kbit/s from CiA 301 timing tests.
 */
const uint32_t can_perf_classic_bitrates[] = {
	CAN_PERF_TEST_BITRATE_1,
	CAN_PERF_TEST_BITRATE_2,
	500000,
	CAN_PERF_TEST_BITRATE_3,
};

const size_t can_perf_classic_bitrate_count = ARRAY_SIZE(can_perf_classic_bitrates);

const uint32_t can_perf_classic_bitrate_short = CAN_PERF_TEST_BITRATE_2;

uint32_t can_perf_cycles_elapsed(uint32_t start, uint32_t end)
{
	if (end >= start) {
		return end - start;
	}

	return (UINT32_MAX - start) + end + 1U;
}

uint32_t can_perf_classic_frame_bits(uint8_t dlc)
{
	uint32_t data_bits = can_dlc_to_bytes(dlc) * 8U;
	uint32_t base_bits = 47U + data_bits;

	return base_bits + (base_bits / 4U);
}

uint32_t can_perf_theoretical_fps(uint32_t bitrate, uint8_t dlc, bool fd, uint32_t bitrate_data)
{
	uint64_t frame_time_ns;

	if (!fd) {
		uint32_t bits = can_perf_classic_frame_bits(dlc);

		frame_time_ns = ((uint64_t)bits * 1000000000ULL) / bitrate;
	} else {
		const uint32_t arb_bits = can_perf_classic_frame_bits(0);
		const uint32_t data_bytes = can_dlc_to_bytes(dlc);
		const uint32_t data_bits = data_bytes * 8U;
		const uint32_t fd_overhead = 41U;
		const uint32_t data_phase_bits = fd_overhead + data_bits + ((fd_overhead + data_bits) / 5U);

		frame_time_ns = ((uint64_t)arb_bits * 1000000000ULL) / bitrate;
		frame_time_ns += ((uint64_t)data_phase_bits * 1000000000ULL) / bitrate_data;
	}

	if (frame_time_ns == 0U) {
		return 0U;
	}

	return (uint32_t)(1000000000ULL / frame_time_ns);
}

void can_perf_fill_result(struct can_perf_result *result, uint32_t frame_count,
			  uint32_t payload_bytes, uint32_t start_cycles, uint32_t end_cycles,
			  uint32_t bitrate, uint8_t dlc, bool fd, uint32_t bitrate_data)
{
	const uint32_t cycles = can_perf_cycles_elapsed(start_cycles, end_cycles);
	const uint64_t elapsed_us = k_cyc_to_us_ceil64(cycles);
	const uint32_t theoretical_fps = can_perf_theoretical_fps(bitrate, dlc, fd, bitrate_data);

	result->frame_count = frame_count;
	result->elapsed_us = elapsed_us;

	if (elapsed_us == 0U) {
		result->frames_per_sec = 0U;
		result->bytes_per_sec = 0U;
		result->bus_efficiency_permille = 0U;
		return;
	}

	result->frames_per_sec = (uint32_t)((uint64_t)frame_count * USEC_PER_SEC / elapsed_us);
	result->bytes_per_sec = (uint32_t)((uint64_t)frame_count * payload_bytes * USEC_PER_SEC /
					   elapsed_us);

	if (theoretical_fps > 0U) {
		result->bus_efficiency_permille =
			(uint32_t)((uint64_t)result->frames_per_sec * 1000ULL / theoretical_fps);
	} else {
		result->bus_efficiency_permille = 0U;
	}
}

void can_perf_print_result(const char *label, const struct can_perf_result *result)
{
	printk("CAN perf: %s: %u frames in %llu us, %u fps (%u B/s), bus_eff=%u permille\n",
	       label, result->frame_count, result->elapsed_us, result->frames_per_sec,
	       result->bytes_per_sec, result->bus_efficiency_permille);
}

void can_perf_assert_thresholds(const char *label, const struct can_perf_result *result,
				uint32_t min_fps)
{
	__ASSERT(result->bus_efficiency_permille >= CONFIG_CAN_PERF_MIN_BUS_EFFICIENCY_PERMILLE,
		 "%s: bus efficiency %u permille below minimum %u", label,
		 result->bus_efficiency_permille, CONFIG_CAN_PERF_MIN_BUS_EFFICIENCY_PERMILLE);

	if (IS_ENABLED(CONFIG_CAN_PERF_STRICT_THRESHOLDS) && min_fps > 0U) {
		__ASSERT(result->frames_per_sec >= min_fps,
			 "%s: %u fps below strict minimum %u", label, result->frames_per_sec,
			 min_fps);
	}
}

int can_perf_setup_loopback(can_mode_t mode)
{
	int err;

	__ASSERT(device_is_ready(can_perf_dev), "CAN device not ready");
	k_object_access_grant(can_perf_dev, k_current_get());

	err = can_stop(can_perf_dev);
	if (err != 0 && err != -EALREADY) {
		return err;
	}

	err = can_set_mode(can_perf_dev, mode);
	if (err != 0) {
		return err;
	}

	__ASSERT(can_get_mode(can_perf_dev) == mode, "CAN mode mismatch after can_set_mode()");

	return can_start(can_perf_dev);
}

static int can_perf_apply_nominal_timing(uint32_t bitrate)
{
	struct can_timing timing = { 0 };
	int sp_err;
	int err;

	err = can_stop(can_perf_dev);
	if (err != 0 && err != -EALREADY) {
		return err;
	}

	sp_err = can_calc_timing(can_perf_dev, &timing, bitrate,
				 CONFIG_CAN_PERF_NOMINAL_SAMPLE_POINT);
	if (sp_err == -ENOTSUP) {
		return -ENOTSUP;
	}
	if (sp_err < 0) {
		return sp_err;
	}
	if (sp_err > CONFIG_CAN_SAMPLE_POINT_MARGIN) {
		return -ERANGE;
	}

	err = can_set_timing(can_perf_dev, &timing);
	if (err != 0) {
		return err;
	}

	return can_start(can_perf_dev);
}

int can_perf_set_bitrate_classic(uint32_t bitrate)
{
	return can_perf_apply_nominal_timing(bitrate);
}

int can_perf_set_bitrate_fd(uint32_t bitrate, uint32_t bitrate_data)
{
	struct can_timing timing = { 0 };
	struct can_timing timing_data = { 0 };
	int sp_err;
	int err;

	err = can_stop(can_perf_dev);
	if (err != 0 && err != -EALREADY) {
		return err;
	}

	sp_err = can_calc_timing(can_perf_dev, &timing, bitrate,
				 CONFIG_CAN_PERF_NOMINAL_SAMPLE_POINT);
	if (sp_err == -ENOTSUP) {
		return -ENOTSUP;
	}
	if (sp_err < 0) {
		return sp_err;
	}
	if (sp_err > CONFIG_CAN_SAMPLE_POINT_MARGIN) {
		return -ERANGE;
	}

	err = can_set_timing(can_perf_dev, &timing);
	if (err != 0) {
		return err;
	}

	sp_err = can_calc_timing_data(can_perf_dev, &timing_data, bitrate_data,
				      CONFIG_CAN_PERF_FD_DATA_SAMPLE_POINT);
	if (sp_err == -ENOTSUP) {
		return -ENOTSUP;
	}
	if (sp_err < 0) {
		return sp_err;
	}
	if (sp_err > CONFIG_CAN_SAMPLE_POINT_MARGIN) {
		return -ERANGE;
	}

	err = can_set_timing_data(can_perf_dev, &timing_data);
	if (err != 0) {
		return err;
	}

	return can_start(can_perf_dev);
}

bool can_perf_prepare_classic(uint32_t bitrate)
{
	int err;

	err = can_perf_setup_loopback(CAN_MODE_LOOPBACK);
	if (err != 0) {
		__ASSERT(false, "failed to set up loopback (err %d)", err);
		return false;
	}

	err = can_perf_set_bitrate_classic(bitrate);
	if (err == -ENOTSUP) {
		printk("CAN perf: nominal bitrate %u not supported, skipping\n", bitrate);
		return false;
	}
	__ASSERT(err == 0, "failed to set nominal timing (err %d)", err);

	return true;
}

bool can_perf_prepare_fd(void)
{
	can_mode_t cap;
	int err;

	err = can_get_capabilities(can_perf_dev, &cap);
	__ASSERT(err == 0, "failed to get capabilities (err %d)", err);

	if ((cap & CAN_MODE_FD) == 0U) {
		printk("CAN perf: FD not supported, skipping\n");
		return false;
	}

	err = can_perf_setup_loopback(CAN_MODE_LOOPBACK | CAN_MODE_FD);
	if (err != 0) {
		__ASSERT(false, "failed to set up FD loopback (err %d)", err);
		return false;
	}

	err = can_perf_set_bitrate_fd(CONFIG_CAN_PERF_FD_BITRATE, CONFIG_CAN_PERF_FD_BITRATE_DATA);
	if (err == -ENOTSUP) {
		printk("CAN perf: FD bitrates not supported, skipping\n");
		return false;
	}
	__ASSERT(err == 0, "failed to set FD timing (err %d)", err);

	return true;
}

void can_perf_teardown(void)
{
	(void)can_stop(can_perf_dev);
}

static int cmp_u64(const void *a, const void *b)
{
	const uint64_t va = *(const uint64_t *)a;
	const uint64_t vb = *(const uint64_t *)b;

	if (va < vb) {
		return -1;
	}
	if (va > vb) {
		return 1;
	}

	return 0;
}

void can_perf_latency_compute_stats(uint64_t *samples, uint32_t count,
				    struct can_perf_latency_stats *stats)
{
	uint64_t sum = 0U;

	stats->samples = count;
	stats->min_us = samples[0];
	stats->max_us = samples[0];

	for (uint32_t i = 0; i < count; i++) {
		sum += samples[i];
		if (samples[i] < stats->min_us) {
			stats->min_us = samples[i];
		}
		if (samples[i] > stats->max_us) {
			stats->max_us = samples[i];
		}
	}

	stats->avg_us = sum / count;

	qsort(samples, count, sizeof(samples[0]), cmp_u64);

	const uint32_t p99_idx = (count * 99U) / 100U;

	stats->p99_us = samples[p99_idx];
}

void can_perf_print_latency_stats(const char *label, const struct can_perf_latency_stats *stats)
{
	printk("CAN perf: %s: %u samples, avg=%llu us, min=%llu us, max=%llu us, "
	       "p99=%llu us\n",
	       label, stats->samples, stats->avg_us, stats->min_us, stats->max_us, stats->p99_us);
}
