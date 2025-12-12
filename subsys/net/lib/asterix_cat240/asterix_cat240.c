/*
 * Copyright (c) 2025 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/posix/arpa/inet.h>
#include "asterix_cat240.h"

/* Speed of light in meters per second */
#define SPEED_OF_LIGHT_M_PER_S  (299792458.0f)

/* FSPEC bit manipulation */
#define FSPEC_SET_BIT(fspec, bit) ((fspec)[(bit) / 8] |= (0x80 >> ((bit) % 8)))
#define FSPEC_SET_FX(fspec, byte) ((fspec)[byte] |= 0x01)
#define FSPEC_HAS_FX(fspec, byte) (((fspec)[byte] & 0x01) != 0)

static inline void write_u24(uint8_t *buf, uint32_t val)
{
    buf[0] = (val >> 16) & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = val & 0xFF;
}

/**
 * @brief FSPEC for Video message
 */
static size_t asterix_cat240_set_fspec(uint8_t *fspec, 
                                const struct asterix_cat240_video_msg *msg)
{
    memset(fspec, 0, 2);
    
    /* First byte */
    FSPEC_SET_BIT(fspec, 0);  /* FRN 1: Data Source */
    FSPEC_SET_BIT(fspec, 1);  /* FRN 2: Message Type */
    FSPEC_SET_BIT(fspec, 2);  /* FRN 3: Video Record Header */
    
    /* FRN 5 or 6: Video Header Nano/Femto */
    if (msg->use_femto) {
        FSPEC_SET_BIT(fspec, 5);
    } else {
        FSPEC_SET_BIT(fspec, 4);
    }
    
    FSPEC_SET_BIT(fspec, 6);  /* FRN 7: Resolution */
    FSPEC_SET_FX(fspec, 0);   /* Need second byte */
    
    /* Second byte */
    FSPEC_SET_BIT(fspec, 7);  /* FRN 8: Counters */
    
    /* FRN 9, 10, or 11: Video Block */
    if (msg->block_type == VIDEO_BLOCK_LOW) {
        FSPEC_SET_BIT(fspec, 8);
    } else if (msg->block_type == VIDEO_BLOCK_MEDIUM) {
        FSPEC_SET_BIT(fspec, 9);
    } else if (msg->block_type == VIDEO_BLOCK_HIGH) {
        FSPEC_SET_BIT(fspec, 10);
    }
    
    if (msg->has_time) {
        FSPEC_SET_BIT(fspec, 11);  /* FRN 12: Time of Day */
    }
    
    return 2;
}

float asterix_cat240_azimuth_to_degrees(uint16_t az_raw)
{
    return (float)az_raw * 360.0f / 65536.0f;
}

double asterix_cat240_calculate_range(uint32_t cell_dur, uint32_t start_rg,
                                      uint32_t nu_cell, bool use_femto)
{
    double time_seconds;
    
    if (use_femto) {
        time_seconds = cell_dur * 1e-15;
    } else {
        time_seconds = cell_dur * 1e-9;
    }
    
    /* Range = (time * c) / 2, where c is speed of light */
    return time_seconds * (start_rg + nu_cell) * SPEED_OF_LIGHT_M_PER_S / 2.0;
}

int asterix_cat240_send_video(const struct asterix_cat240_video_msg *msg,
                                uint8_t *buf, size_t buf_size)
{
    if (!msg || !buf) {
        return -EINVAL;
    }
    
    size_t pos = 0;
    uint8_t fspec[2];
    size_t fspec_size = asterix_cat240_set_fspec(fspec, msg);
    
    /* Start encoding */
    if (buf_size < 3) {
        return -ENOBUFS;
    }
    
    /* Category */
    buf[pos++] = ASTERIX_CAT240;
    
    /* Length (will update at end) */
    size_t len_pos = pos;
    pos += 2;
    
    /* FSPEC */
    if (buf_size < pos + fspec_size) {
        return -ENOBUFS;
    }
    memcpy(&buf[pos], fspec, fspec_size);
    pos += fspec_size;
    
    /* I240/010 - Data Source Identifier */
    if (buf_size < pos + 2) return -ENOBUFS;
    buf[pos++] = msg->data_source.sac;
    buf[pos++] = msg->data_source.sic;
    
    /* I240/000 - Message Type */
    if (buf_size < pos + 1) return -ENOBUFS;
    buf[pos++] = ASTERIX_CAT240_MSG_VIDEO;
    
    /* I240/020 - Video Record Header */
    if (buf_size < pos + 4) return -ENOBUFS;
    uint32_t msg_index_be = htonl(msg->video_header.msg_index);
    memcpy(&buf[pos], &msg_index_be, 4);
    pos += 4;
    
    /* I240/040 or I240/041 - Video Header Nano/Femto */
    if (buf_size < pos + 12) return -ENOBUFS;
    if (msg->use_femto) {
        uint16_t start_az_be = htons(msg->header.femto.start_az);
        uint16_t end_az_be = htons(msg->header.femto.end_az);
        uint32_t start_rg_be = htonl(msg->header.femto.start_rg);
        uint32_t cell_dur_be = htonl(msg->header.femto.cell_dur);
        
        memcpy(&buf[pos], &start_az_be, 2);
        memcpy(&buf[pos + 2], &end_az_be, 2);
        memcpy(&buf[pos + 4], &start_rg_be, 4);
        memcpy(&buf[pos + 8], &cell_dur_be, 4);
    } else {
        uint16_t start_az_be = htons(msg->header.nano.start_az);
        uint16_t end_az_be = htons(msg->header.nano.end_az);
        uint32_t start_rg_be = htonl(msg->header.nano.start_rg);
        uint32_t cell_dur_be = htonl(msg->header.nano.cell_dur);
        
        memcpy(&buf[pos], &start_az_be, 2);
        memcpy(&buf[pos + 2], &end_az_be, 2);
        memcpy(&buf[pos + 4], &start_rg_be, 4);
        memcpy(&buf[pos + 8], &cell_dur_be, 4);
    }
    pos += 12;
    
    /* I240/048 - Video Cells Resolution & Compression */
    if (buf_size < pos + 2) return -ENOBUFS;
    buf[pos] = msg->resolution.compression ? 0x80 : 0x00;
    buf[pos + 1] = msg->resolution.resolution;
    pos += 2;
    
    /* I240/049 - Video Octets & Cells Counters */
    if (buf_size < pos + 5) return -ENOBUFS;
    uint16_t nb_vb_be = htons(msg->counters.nb_vb);
    memcpy(&buf[pos], &nb_vb_be, 2);
    write_u24(&buf[pos + 2], msg->counters.nb_cells);
    pos += 5;
    
    /* Video Block */
    if (msg->block_type == VIDEO_BLOCK_LOW) {
        size_t block_size = 1 + (msg->video_block.low.rep * 4);
        if (buf_size < pos + block_size) return -ENOBUFS;
        buf[pos++] = msg->video_block.low.rep;
        if (msg->video_block.low.data) {
            memcpy(&buf[pos], msg->video_block.low.data, 
                   msg->video_block.low.rep * 4);
            pos += msg->video_block.low.rep * 4;
        }
    } else if (msg->block_type == VIDEO_BLOCK_MEDIUM) {
        size_t block_size = 1 + (msg->video_block.medium.rep * 64);
        if (buf_size < pos + block_size) return -ENOBUFS;
        buf[pos++] = msg->video_block.medium.rep;
        if (msg->video_block.medium.data) {
            memcpy(&buf[pos], msg->video_block.medium.data,
                   msg->video_block.medium.rep * 64);
            pos += msg->video_block.medium.rep * 64;
        }
    } else if (msg->block_type == VIDEO_BLOCK_HIGH) {
        size_t block_size = 1 + (msg->video_block.high.rep * 256);
        if (buf_size < pos + block_size) return -ENOBUFS;
        buf[pos++] = msg->video_block.high.rep;
        if (msg->video_block.high.data) {
            memcpy(&buf[pos], msg->video_block.high.data,
                   msg->video_block.high.rep * 256);
            pos += msg->video_block.high.rep * 256;
        }
    }
    
    /* I240/140 - Time of Day (optional) */
    if (msg->has_time) {
        if (buf_size < pos + 3) return -ENOBUFS;
        write_u24(&buf[pos], msg->time_of_day.time_of_day);
        pos += 3;
    }
    
    /* Update length field */
    uint16_t final_len = (uint16_t)pos;
    uint16_t final_len_be = htons(final_len);
    memcpy(&buf[len_pos], &final_len_be, 2);
    
    return (int)pos;
}
