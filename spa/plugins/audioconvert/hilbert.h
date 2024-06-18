/* Hilbert function */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef HILBERT_H
#define HILBERT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stddef.h>
#include <math.h>

static inline void blackman_window(float *taps, int n_taps)
{
	int n;
	for (n = 0; n < n_taps; n++) {
		float w = 2.0f * M_PIf * n / (n_taps-1);
		taps[n] = 0.3635819f - 0.4891775f * cosf(w)
			+ 0.1365995f * cosf(2 * w) - 0.0106411f * cosf(3 * w);
	}
}

static inline int hilbert_generate(float *taps, int n_taps)
{
	int i;

	if ((n_taps & 1) == 0)
		return -EINVAL;

	for (i = 0; i < n_taps; i++) {
		int k = -(n_taps / 2) + i;
		if (k & 1) {
			float pk = M_PIf * k;
			taps[i] *= (1.0f - cosf(pk)) / pk;
		} else {
			taps[i] = 0.0f;
		}
	}
	return 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HILBERT_H */
