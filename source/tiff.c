/*
 * tiff.c
 *
 * TIFF writing routines for PNG-to-TIFF utility.
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
#include <string.h>
#include <math.h>

#include "ptot.h"

#define DEFINE_ENUMS
#include "errors.h"

#define MAX_TAGS 40

U16 ASCII_tags[N_KEYWORDS] = {
    TIFF_TAG_Artist, TIFF_TAG_Copyright, TIFF_TAG_Software,
    TIFF_TAG_Model, TIFF_TAG_ImageDescription
};

/*
 * Local statics
 */

static int write_tag(U16, int, U32, U8 *);
static int get_tag_pos(U16);
static int write_basic_tags(void);
static int write_strips(void);
static int write_extended_tags(void);
static int write_png_data(void);
static int write_ifd(void);
static void align_file_offset(int);

static struct _tiff_state {
    IMG_INFO *image;
    FILE *outf;
    int tag_count;
    U16 byte_order;
    U32 file_offset;
    U8 ifd[12 * MAX_TAGS];
    U8 *buf;
} ts;

/*
 * Determine what the local byte order is (this is the one we
 * will use for the output TIFF), and verify that we have compiled
 * the correct macros. We're a little more pedantic here than
 * necessary, but if any of this is not exactly right, the whole
 * thing falls apart quietly, so paranoia is justified.
 */

int
get_local_byte_order(
    void)
{
    U8 testbuf[4];
    int byte_order;

    PUT32(testbuf,0x01020304L);

    if (0x01 == *testbuf) {
        byte_order = TIFF_BO_Motorola;

#ifndef BIG_ENDIAN
	ASSERT(FALSE);
#endif
        BE_PUT32(testbuf, 0x01020304L);
        if (0x01020304 != GET32(testbuf)) return ERR_BYTE_ORDER;
        LE_PUT32(testbuf, 0x04030201L);
        if (0x01020304 != GET32(testbuf)) return ERR_BYTE_ORDER;
    } else if (0x04 == *testbuf) {
        byte_order = TIFF_BO_Intel;

#ifndef LITTLE_ENDIAN
	ASSERT(FALSE);
#endif
        LE_PUT32(testbuf, 0x01020304L);
        if (0x01020304 != GET32(testbuf)) return ERR_BYTE_ORDER;
        BE_PUT32(testbuf, 0x04030201L);
        if (0x01020304 != GET32(testbuf)) return ERR_BYTE_ORDER;
    } else return ERR_BYTE_ORDER;

    ASSERT(TIFF_BO_Intel == byte_order || \
      TIFF_BO_Motorola == byte_order);

    return byte_order;
}

/*
 * Write image specified by IMGINFO structure to TIFF file.
 */

int
write_TIFF(
    FILE *outf,
    IMG_INFO *image)
{
    int err;

    ASSERT(NULL != outf);
    ASSERT(NULL != image);
    ASSERT(NULL != image->pixel_data_file);

    if (NULL == (ts.buf = (U8 *)malloc(IOBUF_SIZE)))
      return ERR_MEMORY;
    ts.outf = outf;
    ts.image = image;
    ts.byte_order = get_local_byte_order();

    PUT16(ts.buf, ts.byte_order);
    PUT16(ts.buf+2, TIFF_MagicNumber);
    PUT32(ts.buf+4, 0); /* Will be filled in later */

    if (8 != fwrite(ts.buf, 1, 8, outf)) return ERR_WRITE;
    ts.file_offset = 8;
    ts.tag_count = 0;
    memset(ts.ifd, 0, 12 * MAX_TAGS);

    if (0 != (err = write_basic_tags())) return err;
    if (0 != (err = write_strips())) return err;
    remove(image->pixel_data_file);
    if (0 != (err = write_extended_tags())) return err;

    if (0 != image->png_data_size) {
        ASSERT(NULL != image->png_data_file);
        if (0 != (err = write_png_data())) return err;
        remove(image->png_data_file);
    }
    err = write_ifd();

    free(ts.buf);
    return err;
}

/*
 * Sizes (in bytes) of the respective TIFF data types
 */
static data_sizes[] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8 };

#define DIRENT(index,byte) (ts.ifd+12*(index)+(byte))

/*
 * Find where to insert the new tag into the sorted IFD
 * by simple linear search.
 */

static int
get_tag_pos(
    U16 newtag)
{
    int tag, tagpos, newpos = 0;

    while (newpos < ts.tag_count) {
        tag = GET16(DIRENT(newpos,0));
        if (tag > newtag) break;
        ++newpos;
        ASSERT(newpos < MAX_TAGS);
    }
    for (tagpos = ++ts.tag_count; tagpos > newpos; --tagpos) {
        memcpy(DIRENT(tagpos,0), DIRENT(tagpos-1,0), 12);
    }
    PUT16(DIRENT(newpos,0), newtag);
    return newpos;
}

static int
write_tag(
    U16 newtag,
    int data_type,
    U32 count,
    U8 *buffer)
{
    U32 data_size;
    int newpos;

    ASSERT(ts.tag_count < MAX_TAGS);
    ASSERT(data_type > 0 && data_type <= 12);
    ASSERT(NULL != buffer);
    ASSERT(NULL != ts.outf);

    newpos = get_tag_pos(newtag);
    PUT16(DIRENT(newpos,2), data_type);
    PUT32(DIRENT(newpos,4), count);

    data_size = count * data_sizes[data_type];
    if (data_size <= 4) {
        memcpy(DIRENT(newpos,8), buffer, 4);
        if (data_size < 4)
           memset(DIRENT(newpos,8+data_size), 0,
             (size_t)(4-data_size));
    } else {
        align_file_offset(2);
        PUT32(DIRENT(newpos,8), ts.file_offset);
        fwrite(buffer, data_sizes[data_type], (size_t)count,
          ts.outf);
        ts.file_offset += count * data_sizes[data_type];
    }
    return 0;
}

static int
write_png_data(
    void)
{
    int newpos;
    FILE *inf;
    size_t bytes;

    ASSERT(NULL != ts.image->png_data_file);

    if (NULL == (inf = fopen(ts.image->png_data_file, "rb")))
      return ERR_READ;

    newpos = get_tag_pos(TIFF_TAG_PNGChunks);
    PUT16(DIRENT(newpos,2), TIFF_DT_UNDEFINED);
    PUT32(DIRENT(newpos,4), ts.image->png_data_size);

    align_file_offset(2);
    PUT32(DIRENT(newpos,8), ts.file_offset);
    while (0 != ts.image->png_data_size) {

        bytes = fread(ts.buf, 1, (size_t)min(IOBUF_SIZE,
          ts.image->png_data_size), inf);
        fwrite(ts.buf, 1, bytes, ts.outf);
        ts.image->png_data_size -= bytes;
        ts.file_offset += bytes;
    }
    fclose(inf);
    return 0;
}

#undef DIRENT

/*
 * Some data structures must be at even byte offsets in the
 * file. Some must be aligned on 32-bit boundaries. We handle
 * those cases by simply adding pad bytes where needed.
 */

static void
align_file_offset(
    int modulus)
{
    ASSERT(modulus > 0 && modulus <= 16);

    while (0 != (ts.file_offset % modulus)) {
        putc(0, ts.outf);
        ++ts.file_offset;
    }
}

static int
write_basic_tags(
    void)
{
    int i;
    U16 short_val;

    ASSERT(NULL != ts.buf);
    ASSERT(NULL != ts.image);

    PUT32(ts.buf, ts.image->width);
    write_tag(TIFF_TAG_ImageWidth, TIFF_DT_LONG, 1, ts.buf);

    PUT32(ts.buf, ts.image->height);
    write_tag(TIFF_TAG_ImageLength, TIFF_DT_LONG, 1, ts.buf);

    if (ts.image->is_palette) short_val = TIFF_PI_PLTE;
    else if (ts.image->is_color) short_val = TIFF_PI_RGB;
    else short_val = TIFF_PI_GRAY;
    PUT16(ts.buf, short_val);
    write_tag(TIFF_TAG_PhotometricInterpretation,
      TIFF_DT_SHORT, 1, ts.buf);

    PUT16(ts.buf, TIFF_CT_NONE);
    write_tag(TIFF_TAG_Compression, TIFF_DT_SHORT, 1, ts.buf);

    PUT16(ts.buf, TIFF_PC_CONTIG);
    write_tag(TIFF_TAG_PlanarConfiguration, TIFF_DT_SHORT, 1,
      ts.buf);

    for (i = 0; i < ts.image->samples_per_pixel; ++i) {
        PUT16(ts.buf + 2 * i, ts.image->bits_per_sample);
    }
    write_tag(TIFF_TAG_BitsPerSample, TIFF_DT_SHORT,
      ts.image->samples_per_pixel, ts.buf);

    PUT16(ts.buf, ts.image->samples_per_pixel);
    write_tag(TIFF_TAG_SamplesPerPixel, TIFF_DT_SHORT, 1, ts.buf);

    if (ts.image->is_palette) {
        int index, cmap_size;
        U8 *srcp, *redp, *greenp, *bluep;

        cmap_size = 1 << ts.image->bits_per_sample;
        if (6 * cmap_size > IOBUF_SIZE) return ERR_WRITE;
        memset(ts.buf, 0, 6 * cmap_size);

        srcp = ts.image->palette;
        redp = ts.buf;
        greenp = ts.buf + 2 * cmap_size;
        bluep = ts.buf + 4 * cmap_size;

        for (index = 0; index < ts.image->palette_size; ++index) {
            *redp++ = *srcp;
            *redp++ = *srcp++;
            *greenp++ = *srcp;
            *greenp++ = *srcp++;
            *bluep++ = *srcp;
            *bluep++ = *srcp++;
        }
        write_tag(TIFF_TAG_ColorMap, TIFF_DT_SHORT, 3 * cmap_size,
          ts.buf);
    }
    /*
     * Being truly lossless-minded here, we should check for the
     * transparency information in the structure and expand that
     * into a full alpha channel in the TIFF. This is left as an
     * exercise for the reader. :-)
     */
    if (ts.image->has_alpha /* || ts.image->has_trns */) {
        PUT16(ts.buf, TIFF_ES_UNASSOC);
        write_tag(TIFF_TAG_ExtraSamples, TIFF_DT_SHORT, 1, ts.buf);
    }
    return 0;
}

static int
write_extended_tags(
    void)
{
    int i;
    U16 tiff_unit;
    U32 xoff, yoff, longside, bias;

    tiff_unit = 0xFFFF; /* Not yet assigned */

    if (0 != ts.image->xres) {
        if (PNG_MU_None == ts.image->resolution_unit)
          tiff_unit = TIFF_RU_NONE;
        else {
            ASSERT(PNG_MU_Meter == ts.image->resolution_unit);
            tiff_unit = TIFF_RU_CM;
        }
        PUT16(ts.buf, tiff_unit);
        write_tag(TIFF_TAG_ResolutionUnit, TIFF_DT_SHORT, 1,
          ts.buf);

        PUT32(ts.buf, ts.image->xres);
        PUT32(ts.buf+4, 100L); /* Convert micrometers to cm */
        write_tag(TIFF_TAG_XResolution, TIFF_DT_RATIONAL,
          1, ts.buf);

        PUT32(ts.buf, ts.image->yres);
        PUT32(ts.buf+4, 100L);
        write_tag(TIFF_TAG_YResolution, TIFF_DT_RATIONAL,
          1, ts.buf);
    }
    /*
     * TIFF Assumes the same unit for resolution and offset.
     * PNG does not, so we have to do some converting here.
     * Also, TIFF does not apparently allow offsets when there
     * is no resolution unit (or at least doesn't define that
     * case unambiguously). This is one of the very rare cases
     * where TIFF is inadequately specified.
     */
    if (0 != ts.image->xoffset) {
        if (TIFF_RU_NONE != tiff_unit) {
            if (0xFFFF == tiff_unit) {
                PUT16(ts.buf, tiff_unit = TIFF_RU_CM);
                write_tag(TIFF_TAG_ResolutionUnit, TIFF_DT_SHORT, 1,
                  ts.buf);
            }
            ASSERT(TIFF_RU_CM == tiff_unit);

            xoff = ts.image->xoffset;
            yoff = ts.image->yoffset;

            if (PNG_MU_Micrometer != ts.image->offset_unit) {
                ASSERT(PNG_MU_Pixel == ts.image->offset_unit);

                if (PNG_MU_None == ts.image->resolution_unit) {
                    /*
                     * Assume 72 DPI
                     */
                    xoff = (ts.image->xoffset * 3175) / 9;
                    yoff = (ts.image->yoffset * 3175) / 9;
                } else {
                    /*
                     * Guard against overflow
                     */
                    longside = max(ts.image->xoffset,
                      ts.image->yoffset);
                    bias = 1;

                    while (longside > 2000) {
                        bias *= 2;
                        longside /= 2;
                    }
                    xoff = (ts.image->xoffset * (1000000 / bias)) /
                      (ts.image->xres / bias);
                    yoff = (ts.image->yoffset * (1000000 / bias)) /
                      (ts.image->yres / bias);
                }
            }
            PUT32(ts.buf, xoff);
            PUT32(ts.buf + 4, 10000L);
            write_tag(TIFF_TAG_XPosition, TIFF_DT_RATIONAL, 1,
              ts.buf);

            PUT32(ts.buf, yoff);
            PUT32(ts.buf + 4, 10000L);
            write_tag(TIFF_TAG_YPosition, TIFF_DT_RATIONAL, 1,
              ts.buf);
        }
    }
    /*
     * Map cHRM chunk to WhitePoint and PrimaryChromaticities
     */
    if (0.0 != ts.image->chromaticities[0]) {
        int i;

        for (i = 0; i < 2; ++i) {
            PUT32(ts.buf + 8 * i, ts.image->chromaticities[i]);
            PUT32(ts.buf + 8 * i + 4, 100000L);
        }
        write_tag(TIFF_TAG_WhitePoint, TIFF_DT_RATIONAL, 2,
          ts.buf);

        for (i = 0; i < 6; ++i) {
            PUT32(ts.buf + 8 * i, ts.image->chromaticities[i+2]);
            PUT32(ts.buf + 8 * i + 4, 100000L);
        }
        write_tag(TIFF_TAG_PrimaryChromaticities, TIFF_DT_RATIONAL,
          6, ts.buf);
    }
    /*
     * ASCII Tags
     */
    for (i = 0; i < N_KEYWORDS; ++i) {
        if (NULL != ts.image->keywords[i]) {
            write_tag(ASCII_tags[i], TIFF_DT_ASCII,
              strlen(ts.image->keywords[i]) + 1,
              ts.image->keywords[i]);
        }
    }
    /*
     * Map gAMA chunk to TransferFunction tag
     */
    if (0.0 != ts.image->source_gamma) {
        U32 count, index;
        double maxval;

        count = 1 << ts.image->bits_per_sample;
        if (2 * count > IOBUF_SIZE) return ERR_WRITE;

        PUT16(ts.buf, 0);
        maxval = (double)count - 1.0;

        for (index = 1; index < count; ++index) {
            PUT16(ts.buf + 2 * index,
              (U16)floor(0.5 + 65535 * pow((double)index / maxval,
              1.0 / ts.image->source_gamma)));
        }
        write_tag(TIFF_TAG_TransferFunction, TIFF_DT_SHORT,
          count, ts.buf);
    }
    return 0;
}

static int
write_ifd(
    void)
{
    int err;

    ASSERT(NULL != ts.buf);
    ASSERT(NULL != ts.outf);
    ASSERT(ts.tag_count <= MAX_TAGS);

    align_file_offset(2);
    if (ts.file_offset == (U32)ftell(ts.outf)) err = 0;
    else err = ERR_WRITE;

    PUT16(ts.buf, ts.tag_count);
    fwrite(ts.buf, 2, 1, ts.outf);
    fwrite(ts.ifd, 12, ts.tag_count, ts.outf);
    PUT32(ts.buf, 0L);
    fwrite(ts.buf, 4, 1, ts.outf);

    PUT32(ts.buf, ts.file_offset);
    fseek(ts.outf, 4L, SEEK_SET);
    fwrite(ts.buf, 1, 4, ts.outf);

    return 0;
}

/*
 * Write out the actual pixel data into approximately 8k strips
 * (larger if needed to fit the StripOffsets data into one I/O
 * buffer) and write the related tags.
 */

#define BPS (ts.image->bits_per_sample)
#define SPP (ts.image->samples_per_pixel)
#define OKW(x) ((x)<ts.image->width)

static int
write_strips(
    void)
{
    size_t line_size, strip_size;
    U32 strip, total_strips, rows_per_strip;
    U8 *line_buf;
    FILE *inf;

    line_size = new_line_size(ts.image, 0, 1);
    if (line_size > 4096) {
        rows_per_strip = 1;
    } else {
        rows_per_strip = 8192 / line_size;
    }
    ASSERT(0 != rows_per_strip);

    do {
        strip_size = rows_per_strip * line_size;
        total_strips = (ts.image->height + (rows_per_strip - 1)) /
          rows_per_strip;
        rows_per_strip *= 2;
    } while (4 * total_strips > IOBUF_SIZE);
    rows_per_strip /= 2;

    PUT32(ts.buf, rows_per_strip);
    write_tag(TIFF_TAG_RowsPerStrip, TIFF_DT_LONG, 1, ts.buf);

    for (strip = 0; strip < total_strips; ++strip) {
        PUT32(ts.buf + 4 * strip, strip_size);
    }
    write_tag(TIFF_TAG_StripByteCounts, TIFF_DT_LONG,
      total_strips, ts.buf);

    align_file_offset(2);
    if (0 != (strip_size & 1)) ++strip_size;

    for (strip = 0; strip < total_strips; ++strip) {
        PUT32(ts.buf + 4 * strip, ts.file_offset +
          4 * total_strips + strip * strip_size);
    }
    write_tag(TIFF_TAG_StripOffsets, TIFF_DT_LONG,
      total_strips, ts.buf);
    /*
     * Write the strip data from the pixel data file.
     */
    if (NULL == (line_buf = (U8 *)malloc(line_size)))
      return ERR_MEMORY;

    ASSERT(NULL != ts.image->pixel_data_file);
    if (NULL == (inf = fopen(ts.image->pixel_data_file, "rb"))) {
        free(line_buf);
        return ERR_READ;
    }
    for (strip = 0; strip < total_strips; ++strip) {
        U32 row, col, scanline;

        scanline = 0;
        align_file_offset(2);

        for (row = 0; row < rows_per_strip; ++row) {
            int bit, byte, step, sample;
            U16 word;
            U8 *lp;

            lp = line_buf;
            if (BPS < 8) step = 8 / BPS;
            else step = 1;

            for (col = 0; col < ts.image->width; col += step) {
                switch (BPS) {
                case 1:
                    ASSERT(1 == SPP);

                    *lp = getc(inf) & 0x80;
                    for (bit = 1; bit < 8; ++bit) {
                        if (!OKW(col+bit)) break;
                        byte = getc(inf);
                        if (0 != (byte & 0x80)) {
                            *lp |= (1 << (7 - bit));
                        }
                    }
                    ++lp;
                    break;
                case 2:
                    ASSERT(1 == SPP);

                    *lp = getc(inf) & 0xC0;
                    if OKW(col+1) *lp |= ((getc(inf) >> 2) & 0x30);
                    if OKW(col+2) *lp |= ((getc(inf) >> 4) & 0x0C);
                    if OKW(col+3) *lp |= ((getc(inf) >> 6) & 0x03);
                    ++lp;
                    break;
                case 4:
                    ASSERT(1 == SPP);

                    *lp = getc(inf) & 0xF0;
                    if OKW(col+1) *lp |= ((getc(inf) >> 4) & 0x0F);
                    ++lp;
                    break;
                case 8:
                    for (sample = 0; sample < SPP; ++sample) {
                        *lp++ = (0xFF & getc(inf));
                    }
                    break;
                case 16:
                    for (sample = 0; sample < SPP; ++sample) {
                         word = ((getc(inf) << 8) & 0xFF00);
                         word |= (0xFF & getc(inf));
                         PUT16(lp, word);
                         lp += 2;
                    }
                    break;
                default:
                    ASSERT(FALSE);
                }
            }
            ASSERT(lp - line_buf == line_size);
            if (line_size != fwrite(line_buf, 1, line_size, ts.outf)) {
                fclose(inf);
                free(line_buf);
                return ERR_WRITE;
            }
            ts.file_offset += line_size;
            if (++scanline >= ts.image->height) break;
        }
    }
    fclose(inf);
    free(line_buf);
    return 0;
}

#undef SPP
#undef BPS
#undef OKW

/*
 * End of TIFF.C
 */

