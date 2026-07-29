/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "timing.h"

#include <dk_buttons_and_leds.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>

#if IS_ENABLED(CONFIG_CPU_LOAD)
#include "cpu_load_monitor.h"
#endif

#define LATENCY_MSGQ_DEPTH 4

CAN_MSGQ_DEFINE(can_perf_msgq, LATENCY_MSGQ_DEPTH);

static struct k_sem tx_done_sem;

static void tx_done_callback(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	__ASSERT(error == 0, "TX callback reported error %d", error);
	k_sem_give(&tx_done_sem);
}

static struct can_frame can_perf_classic_frame(void)
{
	struct can_frame frame = {
		.flags = 0U,
		.id = CAN_PERF_STD_ID,
		.dlc = 8,
		.data = {1, 2, 3, 4, 5, 6, 7, 8},
	};

	return frame;
}

static int can_perf_send_frame(const struct can_frame *frame, k_timeout_t timeout,
			       can_tx_callback_t callback, void *user_data)
{
	int err;

	err = can_send(can_perf_dev, frame, timeout, callback, user_data);
	__ASSERT(err != -EBUSY, "arbitration lost in loopback mode");
	return err;
}

static void bench_phase_begin(void)
{
#if IS_ENABLED(CONFIG_CPU_LOAD)
	cpu_load_monitor_start();
#endif
	dk_set_led_on(DK_LED1);
}

static void bench_phase_end(void)
{
	dk_set_led_off(DK_LED1);
#if IS_ENABLED(CONFIG_CPU_LOAD)
	cpu_load_monitor_stop();
	cpu_load_monitor_show();
#endif
}

int can_perf_bench_init(void)
{
	int err;

	__ASSERT(device_is_ready(can_perf_dev), "CAN device not ready");
	k_object_access_grant(can_perf_dev, k_current_get());
	k_object_access_grant(&can_perf_msgq, k_current_get());

	err = dk_leds_init();
	if (err != 0) {
		return err;
	}

#if IS_ENABLED(CONFIG_CPU_LOAD)
	cpu_load_monitor_init();
#endif

	return 0;
}

void can_perf_idle_window(const char *label)
{
	can_perf_teardown();
	printk("CAN perf: idle window: %s\n", label);
	k_msleep(CONFIG_CAN_PERF_DEAD_TIME_MS);
}

static void throughput_test_common(uint32_t bitrate, uint8_t dlc, bool fd, uint32_t bitrate_data,
				   const char *label, uint32_t min_fps)
{
	struct can_perf_result result;
	struct can_frame frame = can_perf_classic_frame();
	uint32_t start;
	uint32_t end;
	int err;

	if (fd) {
		frame.flags = CAN_FRAME_FDF | CAN_FRAME_BRS;
		frame.dlc = 0xf;
	}

	k_sem_init(&tx_done_sem, 0, CONFIG_CAN_PERF_FRAME_COUNT);

	if (fd) {
		if (!can_perf_prepare_fd()) {
			return;
		}
	} else if (!can_perf_prepare_classic(bitrate)) {
		return;
	}

	bench_phase_begin();
	start = k_cycle_get_32();

	for (int i = 0; i < CONFIG_CAN_PERF_FRAME_COUNT; i++) {
		err = can_perf_send_frame(&frame, CAN_PERF_SEND_TIMEOUT, tx_done_callback, NULL);
		__ASSERT(err == 0, "can_send failed (err %d)", err);

		err = k_sem_take(&tx_done_sem, CAN_PERF_SEND_TIMEOUT);
		__ASSERT(err == 0, "TX completion timeout");
	}

	end = k_cycle_get_32();
	bench_phase_end();

	can_perf_teardown();

	can_perf_fill_result(&result, CONFIG_CAN_PERF_FRAME_COUNT, can_dlc_to_bytes(dlc), start, end,
			     bitrate, dlc, fd, bitrate_data);
	can_perf_print_result(label, &result);
	printk("### Summary ###\n");
	can_perf_assert_thresholds(label, &result, min_fps);
}

void can_perf_run_tx_throughput_classic(uint32_t bitrate)
{
	throughput_test_common(bitrate, 8, false, 0, "tx_throughput_classic",
			       CONFIG_CAN_PERF_MIN_TX_FPS_CLASSIC);
}

void can_perf_run_tx_throughput_fd(void)
{
	throughput_test_common(CONFIG_CAN_PERF_FD_BITRATE, 0xf, true,
			       CONFIG_CAN_PERF_FD_BITRATE_DATA, "tx_throughput_fd",
			       CONFIG_CAN_PERF_MIN_TX_FPS_FD);
}

void can_perf_run_single_frame_tx_latency(uint32_t bitrate)
{
	struct can_frame frame = can_perf_classic_frame();
	uint64_t tx_us;
	int err;

	k_sem_init(&tx_done_sem, 0, 1);

	if (!can_perf_prepare_classic(bitrate)) {
		return;
	}

	bench_phase_begin();

	uint32_t start = k_cycle_get_32();

	err = can_perf_send_frame(&frame, CAN_PERF_SEND_TIMEOUT, tx_done_callback, NULL);
	__ASSERT(err == 0, "can_send failed (err %d)", err);

	err = k_sem_take(&tx_done_sem, CAN_PERF_SEND_TIMEOUT);
	__ASSERT(err == 0, "TX completion timeout");

	tx_us = k_cyc_to_us_ceil64(can_perf_cycles_elapsed(start, k_cycle_get_32()));
	bench_phase_end();

	can_perf_teardown();

	printk("CAN perf: single_frame_tx @ %u bps: %llu us\n", bitrate, tx_us);
	printk("### Summary ###\n");
}

void can_perf_run_single_frame_rx_latency(uint32_t bitrate)
{
	struct can_frame frame = can_perf_classic_frame();
	struct can_filter filter = {
		.flags = 0U,
		.id = CAN_PERF_STD_ID,
		.mask = CAN_STD_ID_MASK,
	};
	struct can_frame rx_frame;
	uint64_t rx_path_us;
	int filter_id;
	int err;

	k_sem_init(&tx_done_sem, 0, 1);

	if (!can_perf_prepare_classic(bitrate)) {
		return;
	}

	filter_id = can_add_rx_filter_msgq(can_perf_dev, &can_perf_msgq, &filter);
	__ASSERT(filter_id >= 0, "failed to add RX msgq filter (err %d)", filter_id);

	bench_phase_begin();

	err = can_perf_send_frame(&frame, CAN_PERF_SEND_TIMEOUT, tx_done_callback, NULL);
	__ASSERT(err == 0, "can_send failed (err %d)", err);

	err = k_sem_take(&tx_done_sem, CAN_PERF_SEND_TIMEOUT);
	__ASSERT(err == 0, "TX completion timeout");

	uint32_t rx_start = k_cycle_get_32();

	err = k_msgq_get(&can_perf_msgq, &rx_frame, CAN_PERF_RECV_TIMEOUT);
	__ASSERT(err == 0, "RX msgq timeout");

	rx_path_us = k_cyc_to_us_ceil64(can_perf_cycles_elapsed(rx_start, k_cycle_get_32()));
	bench_phase_end();

	can_remove_rx_filter(can_perf_dev, filter_id);
	can_perf_teardown();

	printk("CAN perf: single_frame_rx_path @ %u bps: %llu us (TX complete to msgq)\n", bitrate,
	       rx_path_us);
	printk("### Summary ###\n");
}

void can_perf_run_roundtrip_latency(uint32_t bitrate)
{
	struct can_frame frame = can_perf_classic_frame();
	struct can_filter filter = {
		.flags = 0U,
		.id = CAN_PERF_STD_ID,
		.mask = CAN_STD_ID_MASK,
	};
	uint64_t samples[CONFIG_CAN_PERF_LATENCY_SAMPLES];
	struct can_perf_latency_stats stats;
	struct can_frame rx_frame;
	uint32_t rx_overruns;
	int filter_id;
	int err;

	k_sem_init(&tx_done_sem, 0, 1);

	if (!can_perf_prepare_classic(bitrate)) {
		return;
	}

	filter_id = can_add_rx_filter_msgq(can_perf_dev, &can_perf_msgq, &filter);
	__ASSERT(filter_id >= 0, "failed to add RX msgq filter (err %d)", filter_id);

	bench_phase_begin();

	for (uint32_t i = 0; i < CONFIG_CAN_PERF_LATENCY_SAMPLES; i++) {
		uint32_t start = k_cycle_get_32();

		err = can_perf_send_frame(&frame, CAN_PERF_SEND_TIMEOUT, tx_done_callback, NULL);
		__ASSERT(err == 0, "can_send failed (err %d)", err);

		err = k_msgq_get(&can_perf_msgq, &rx_frame, CAN_PERF_RECV_TIMEOUT);
		__ASSERT(err == 0, "RX msgq timeout");

		samples[i] = k_cyc_to_us_ceil64(can_perf_cycles_elapsed(start, k_cycle_get_32()));
	}

	bench_phase_end();

	can_perf_latency_compute_stats(samples, CONFIG_CAN_PERF_LATENCY_SAMPLES, &stats);

	can_remove_rx_filter(can_perf_dev, filter_id);
	can_perf_teardown();

	can_perf_print_latency_stats("roundtrip_latency", &stats);
	printk("### Summary ###\n");

	__ASSERT(stats.avg_us < CONFIG_CAN_PERF_MAX_AVG_LATENCY_US,
		 "average latency %llu us exceeds %d us", stats.avg_us,
		 CONFIG_CAN_PERF_MAX_AVG_LATENCY_US);

	if (IS_ENABLED(CONFIG_CAN_STATS)) {
		rx_overruns = can_stats_get_rx_overruns(can_perf_dev);
		__ASSERT(rx_overruns == 0U, "RX overruns detected (%u)", rx_overruns);
	}
}

void can_perf_run_sustained_roundtrip(uint32_t bitrate)
{
	struct can_frame frame = can_perf_classic_frame();
	struct can_filter filter = {
		.flags = 0U,
		.id = CAN_PERF_STD_ID,
		.mask = CAN_STD_ID_MASK,
	};
	uint64_t samples[CONFIG_CAN_PERF_SUSTAINED_ROUNDTRIP_COUNT];
	struct can_perf_latency_stats stats;
	struct can_frame rx_frame;
	int filter_id;
	int err;

	k_sem_init(&tx_done_sem, 0, 1);

	if (!can_perf_prepare_classic(bitrate)) {
		return;
	}

	filter_id = can_add_rx_filter_msgq(can_perf_dev, &can_perf_msgq, &filter);
	__ASSERT(filter_id >= 0, "failed to add RX msgq filter (err %d)", filter_id);

	bench_phase_begin();

	for (uint32_t i = 0; i < CONFIG_CAN_PERF_SUSTAINED_ROUNDTRIP_COUNT; i++) {
		uint32_t start = k_cycle_get_32();

		err = can_perf_send_frame(&frame, CAN_PERF_SEND_TIMEOUT, tx_done_callback, NULL);
		__ASSERT(err == 0, "can_send failed (err %d)", err);

		err = k_sem_take(&tx_done_sem, CAN_PERF_SEND_TIMEOUT);
		__ASSERT(err == 0, "TX completion timeout");

		err = k_msgq_get(&can_perf_msgq, &rx_frame, CAN_PERF_RECV_TIMEOUT);
		__ASSERT(err == 0, "RX msgq timeout");
		__ASSERT(rx_frame.id == frame.id, "unexpected frame ID");

		samples[i] = k_cyc_to_us_ceil64(can_perf_cycles_elapsed(start, k_cycle_get_32()));
	}

	bench_phase_end();

	can_perf_latency_compute_stats(samples, CONFIG_CAN_PERF_SUSTAINED_ROUNDTRIP_COUNT, &stats);

	can_remove_rx_filter(can_perf_dev, filter_id);
	can_perf_teardown();

	can_perf_print_latency_stats("sustained_roundtrip", &stats);
	printk("### Summary ###\n");
}

void can_perf_run_rx_burst(uint32_t bitrate)
{
	struct can_frame frame = can_perf_classic_frame();
	struct can_filter filter = {
		.flags = 0U,
		.id = CAN_PERF_STD_ID,
		.mask = CAN_STD_ID_MASK,
	};
	struct can_frame rx_frame;
	uint32_t rx_overruns;
	int filter_id;
	int err;

	k_sem_init(&tx_done_sem, 0, CONFIG_CAN_PERF_BURST_FRAME_COUNT);

	if (!can_perf_prepare_classic(bitrate)) {
		return;
	}

	filter_id = can_add_rx_filter_msgq(can_perf_dev, &can_perf_msgq, &filter);
	__ASSERT(filter_id >= 0, "failed to add RX msgq filter (err %d)", filter_id);

	bench_phase_begin();

	for (int i = 0; i < CONFIG_CAN_PERF_BURST_FRAME_COUNT; i++) {
		err = can_perf_send_frame(&frame, CAN_PERF_SEND_TIMEOUT, tx_done_callback, NULL);
		__ASSERT(err == 0, "can_send failed (err %d)", err);

		err = k_sem_take(&tx_done_sem, CAN_PERF_SEND_TIMEOUT);
		__ASSERT(err == 0, "TX completion timeout");
	}

	for (int i = 0; i < CONFIG_CAN_PERF_BURST_FRAME_COUNT; i++) {
		err = k_msgq_get(&can_perf_msgq, &rx_frame, CAN_PERF_RECV_TIMEOUT);
		__ASSERT(err == 0, "RX msgq timeout");
		__ASSERT(rx_frame.id == frame.id, "unexpected frame ID");
	}

	bench_phase_end();

	can_remove_rx_filter(can_perf_dev, filter_id);
	can_perf_teardown();

	if (IS_ENABLED(CONFIG_CAN_STATS)) {
		rx_overruns = can_stats_get_rx_overruns(can_perf_dev);
		printk("CAN perf: burst: rx_overruns=%u\n", rx_overruns);
		__ASSERT(rx_overruns == 0U, "RX overruns after burst (%u)", rx_overruns);
	}

	printk("### Summary ###\n");
}
