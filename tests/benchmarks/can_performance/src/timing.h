/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef TIMING_H_
#define TIMING_H_

#include <stdint.h>

#include "perf_common.h"

int can_perf_bench_init(void);

void can_perf_idle_window(const char *label);

void can_perf_run_tx_throughput_classic(uint32_t bitrate);

void can_perf_run_tx_throughput_fd(void);

void can_perf_run_single_frame_tx_latency(uint32_t bitrate);

void can_perf_run_single_frame_rx_latency(uint32_t bitrate);

void can_perf_run_roundtrip_latency(uint32_t bitrate);

void can_perf_run_sustained_roundtrip(uint32_t bitrate);

void can_perf_run_rx_burst(uint32_t bitrate);

#endif /* TIMING_H_ */
