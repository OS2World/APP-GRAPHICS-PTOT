/*
 * crc32.c
 *
 * Function to calculate 32-bit CRC values for PNG chunks.
 *
 **********
 *
 * HISTORY
 *
 * 95-03-10 Created by Lee Daniel Crocker <lee@piclab.com>
 *          <URL:http://www.piclab.com/piclab/index.html>
 */

#include <stdlib.h>
#include <stdio.h>

#include "ptot.h"

static U32 crc_table[256] = { 0xFFFFFFFFL };
static void build_crc_table(U32 *);

U32
update_crc(
    U32 input_crc,
    U8 *data,
    U32 count)
{
    U32 crc, byte;

    ASSERT(NULL != data);

    if (0xFFFFFFFFL == *crc_table) build_crc_table(crc_table);

    ASSERT(0x2D02EF8DL == crc_table[255]);
    crc = input_crc;

    for (byte = 0; byte < count; ++byte) {
        crc = ((crc >> 8) & 0xFFFFFFL) ^
          crc_table[ (crc ^ data[byte]) & 0xFF ];
    }
    return crc;
}

static void
build_crc_table(
    U32 *table)
{
    int byte, bit;
    U32 accum;

    ASSERT(NULL != table);

    for (byte = 0; byte < 256; ++byte) {
        accum = byte;

        for (bit = 0; bit < 8; ++bit) {
            if (accum & 1) accum = (accum >> 1) ^ 0xEDB88320L;
            else accum >>= 1;
        }
        table[byte] = accum;
    }
    ASSERT(0x2D02EF8DL == table[255]);
}
