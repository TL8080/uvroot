/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OVERLAY_SHA256_H
#define OVERLAY_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct OverlaySha256 {
    uint32_t state[8];
    uint64_t length;
    unsigned char buffer[64];
    size_t used;
} OverlaySha256;

void overlay_sha256_init(OverlaySha256 *ctx);
void overlay_sha256_update(OverlaySha256 *ctx, const void *data, size_t size);
void overlay_sha256_final(OverlaySha256 *ctx, unsigned char digest[32]);
void overlay_sha256_hex(const unsigned char digest[32], char hex[65]);

#endif
