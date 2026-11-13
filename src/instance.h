/* SPDX-License-Identifier: MIT */
#pragma once

/* Set the thread name so Payload Manager can identify this payload. */
void instance_claim_name(void);

/*
 * Stop any already running instance of this payload.
 * Returns the number of instances stopped, or -1 when the process list
 * cannot be read.
 */
int instance_take_over(void);
