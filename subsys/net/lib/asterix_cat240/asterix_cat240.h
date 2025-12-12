/*
 * Copyright (c) 2025 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implementation based on EUROCONTROL-SPEC-0149-240 Edition 1.3
 * This specification is publicly available and free to implement.
 * 
 */

#ifndef ASTERIX_CAT240_H
#define ASTERIX_CAT240_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ASTERIX Cat 240 Category Code */
#define ASTERIX_CAT240 240

/* Message Types */
enum asterix_cat240_msg_type {
    ASTERIX_CAT240_MSG_TYPE_SUMMARY = 1,
    ASTERIX_CAT240_MSG_TYPE_VIDEO   = 2,
};

/* Video Resolution Encoding */
enum asterix_cat240_resolution {
    ASTERIX_CAT240_RES_MONOBIT = 1, /* 1 bit per cell */
    ASTERIX_CAT240_RES_LOW,         /* 2 bits per cell */
    ASTERIX_CAT240_RES_MEDIUM,      /* 4 bits per cell */
    ASTERIX_CAT240_RES_HIGH,        /* 8 bits per cell */
    ASTERIX_CAT240_RES_VERY_HIGH,   /* 16 bits per cell */
    ASTERIX_CAT240_RES_ULTRA_HIGH,  /* 32 bits per cell */
};

/* Field Reference Numbers (FRN) for UAP */
enum asterix_cat240_frn {
    ASTERIX_CAT240_FRN_DATA_SOURCE = 1,
    ASTERIX_CAT240_FRN_MSG_TYPE,
    ASTERIX_CAT240_FRN_VIDEO_HEADER,
    ASTERIX_CAT240_FRN_VIDEO_SUMMARY,
    ASTERIX_CAT240_FRN_HEADER_NANO,
    ASTERIX_CAT240_FRN_HEADER_FEMTO,
    ASTERIX_CAT240_FRN_RESOLUTION,
    ASTERIX_CAT240_FRN_COUNTERS,
    ASTERIX_CAT240_FRN_BLOCK_LOW,
    ASTERIX_CAT240_FRN_BLOCK_MEDIUM,
    ASTERIX_CAT240_FRN_BLOCK_HIGH,
    ASTERIX_CAT240_FRN_TIME_OF_DAY,
};

/* ASTERIX Data Block Header */
struct asterix_cat240_block_header {
    uint8_t cat;     /* Should be 240 */
    uint16_t len;      /* Total length including CAT and LEN */
    uint16_t fspec;    /* Field Specifier */
};

/* ASTERIX Video Message Structure */
struct asterix_cat240_video_msg {
    uint8_t sac;  /* System Area Code */
    uint8_t sic;  /* System Identification Code */

    uint8_t msg_type;

    uint32_t video_rec_hdr;

    uint16_t start_az;    /* Start azimuth, LSB = 360/65536 degrees */
    uint16_t end_az;      /* End azimuth, LSB = 360/65536 degrees */
    uint32_t start_rg;    /* Starting range in number of cells */
    uint32_t cell_dur;    /* Cell duration in nanoseconds */

    uint8_t cell_res;   /* Video resolution */
    uint8_t compression; /* Compression type */

    uint16_t nb_vb;       /* Number of valid octets */
    uint32_t nb_cells;    /* Number of valid cells (24-bit) */

    uint8_t counter;      /* Repetition factor (blocks of 4 octets) */
    uint8_t *data;        /* Video data */

    uint32_t time_of_day;
} __packed;

struct asterix_cat240_ctx {
    struct asterix_cat240_block_header header;
    struct asterix_cat240_video_msg video_msg;
};

/**
 * @brief Convert azimuth from internal format to degrees
 * 
 * @param az_raw Raw azimuth value (16-bit)
 * @return Azimuth in degrees (0-360)
 */
float asterix_cat240_azimuth_to_degrees(uint16_t az_raw);

/**
 * @brief Calculate cell range in meters
 * 
 * @param cell_dur Cell duration in nanoseconds (or femtoseconds if use_femto=true)
 * @param start_rg Starting range cell number
 * @param nu_cell Cell position in data stream (0-based)
 * @param use_femto True if cell_dur is in femtoseconds
 * @return Range in meters
 */
double asterix_cat240_calculate_range(uint32_t cell_dur, uint32_t start_rg,
                                      uint32_t nu_cell, bool use_femto);

/**
 * @brief Get bits per cell for a given resolution
 * 
 * @param resolution Resolution enum value
 * @return Number of bits per cell, or 0 if invalid
 */
uint8_t asterix_cat240_get_bits_per_cell(enum asterix_cat240_resolution res);

/**
 * @brief Encode a Video message
 * 
 * @param msg Pointer to video message structure
 * @param buf Output buffer
 * @param buf_size Size of output buffer
 * @return Number of bytes encoded, or negative error code
 */
int asterix_cat240_send_video(const struct asterix_cat240_video_msg *msg,
                                uint8_t *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* ASTERIX_CAT240_H */
