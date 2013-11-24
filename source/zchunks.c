/*
 * zchunks.c
 *
 * Code for handling deflated chunks (IDAT and zTXt) is naturally
 * much larger than that for all the other chunks, so I move it all
 * here (as well as tEXt, which shares code with zTXt).
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

#include "ptot.h"

#define DEFINE_ENUMS
#include "errors.h"

extern PNG_STATE ps;

/*
 * Interlacing tables
 */
static int
    interlace_pattern[8][8] = {
        { 0, 5, 3, 5, 1, 5, 3, 5 },
        { 6, 6, 6, 6, 6, 6, 6, 6 },
        { 4, 5, 4, 5, 4, 5, 4, 5 },
        { 6, 6, 6, 6, 6, 6, 6, 6 },
        { 2, 5, 3, 5, 2, 5, 3, 5 },
        { 6, 6, 6, 6, 6, 6, 6, 6 },
        { 4, 5, 4, 5, 4, 5, 4, 5 },
        { 6, 6, 6, 6, 6, 6, 6, 6 } },
    starting_row[7] =   { 0, 0, 4, 0, 2, 0, 1 },
    starting_col[7] =   { 0, 4, 0, 2, 0, 1, 0 },
    row_increment[7] =  { 8, 8, 8, 4, 4, 2, 2 },
    col_increment[7] =  { 8, 8, 4, 4, 2, 2, 1 };

static int zlib_start(void);
static void zlib_end(void);
static void unfilter(int);
static void write_byte(void);
static int repack_tempfiles(void);

/*
 * Decode IDAT chunk. Most of the real work is done inside
 * the NEXTBYTE and FLUSH macros that interface with inflate.c.
 */

#define IS_ZTXT (PNG_CN_zTXt == ps.current_chunk_name)
#define IS_TEXT (PNG_CN_tEXt == ps.current_chunk_name)
#define IS_IDAT (PNG_CN_IDAT == ps.current_chunk_name)

int
decode_IDAT(
    void)
{
    int err, bpp, pass;
    /*
     * Palette chunk must appear before IDAT for palette-
     * based images.  This is technically a fatal error
     * in the PNG, but we will process the image anyway
     * as a grayscale so the user can see _something_.
     */
    if (ps.image->is_palette && (0 == ps.image->palette_size)) {
        print_warning(WARN_NO_PLTE);
        ps.image->is_palette = ps.image->is_color = FALSE;
    }
    ps.got_first_idat = TRUE;
    bpp = ps.image->bits_per_sample / 8;
    if (0 == bpp) bpp = 1;
    ps.byte_offset = ps.image->samples_per_pixel * bpp;
    /*
     * Allocate largest line needed for filtering
     */
    ps.line_size = new_line_size(ps.image, 0, 1);
    ps.this_line = (U8 *)malloc(ps.line_size);
    ps.last_line = (U8 *)malloc(ps.line_size);

    if (NULL == ps.this_line || NULL == ps.last_line) {
        err = ERR_MEMORY;
        goto di_err_out;
    }
    memset(ps.this_line, 0, ps.line_size);
    memset(ps.last_line, 0, ps.line_size);

    ps.current_row = ps.interlace_pass = ps.line_x = 0;
    ps.cur_filter = 255;

    ps.bytes_in_buf = 0L;   /* Required before calling NEXTBYTE */
    ps.bufp = ps.buf;

    if (0 != (err = zlib_start())) goto di_err_out;
    if (0 != (err = create_tempfile(0))) goto di_err_out;

    if (ps.image->is_interlaced) {
        for (pass = 1; pass <= 6; ++pass) {
            if (0 != (err = create_tempfile(pass)))
              goto di_err_out;
        }
        ps.line_size = new_line_size(ps.image, 0, 8);
    } else {
        ps.line_size = new_line_size(ps.image, 0, 1);
    }
    if (0 != (err = inflate())) goto di_err_out;

    close_all_tempfiles();
    err = repack_tempfiles();
di_err_out:
    if (NULL != ps.this_line) free(ps.this_line);
    if (NULL != ps.last_line) free(ps.last_line);

    zlib_end();
    return err;
}

/*
 * Assume that the next byte to read in the file begins the
 * compressed area of an IDAT or zTXt. Set up the necessary
 * structures for decompression.
 */

static int
zlib_start(
    void)
{
    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);

    ps.sum1 = 1;    /* Precondition Adler checksum */
    ps.sum2 = 0;
    ps.inflate_flags = (NEXTBYTE << 8);
    ps.inflate_flags |= NEXTBYTE;

    ps.inflate_window_size =
      1L << (((ps.inflate_flags >> 12) & 0x0F) + 8);

    if (ps.inflate_window_size > 32768) return ERR_COMP_HDR;

    if ( (0 != (ps.inflate_flags % 31)) ||
      (8 != ((ps.inflate_flags >> 8) & 0x0F)) ||
      (0 != (ps.inflate_flags & 0x0020)) ) return ERR_COMP_HDR;

    ps.inflate_window =
      (U8 *)malloc((size_t)(ps.inflate_window_size));
    if (NULL == ps.inflate_window) return ERR_MEMORY;

    ps.inflated_chunk_size = 0L;
    return 0;
}

/*
 * Clean up decompressor and verify checksum.
 */

static void
zlib_end(
    void)
{
    U16 sum1, sum2;

    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);

    if (NULL == ps.inflate_window) return;
    free(ps.inflate_window);

    sum2 = NEXTBYTE << 8;
    sum2 |= NEXTBYTE;
    sum1 = NEXTBYTE << 8;
    sum1 |= NEXTBYTE;

    if ((sum1 != ps.sum1) || (sum2 != ps.sum2))
      print_warning(WARN_BAD_SUM);
}

/*
 * Unfilter the image data byte passed in, and put it into the
 * ps.this_line[] array for write_pixel to find.
 */

static void
unfilter(
    int inbyte)
{
    int prediction, pA, pB, pC, dA, dB, dC;

    ASSERT(NULL != ps.this_line);
    ASSERT(NULL != ps.last_line);

    if (PNG_PF_None == ps.cur_filter) prediction = 0;
    else {
        pA = ((ps.line_x < ps.byte_offset) ? 0 :
          ps.this_line[ps.line_x - ps.byte_offset]);
        pB = ps.last_line[ps.line_x];
        pC = ((ps.line_x < ps.byte_offset) ? 0 :
          ps.last_line[ps.line_x - ps.byte_offset]);

        switch (ps.cur_filter) {
        case PNG_PF_Sub:
            prediction = pA;
            break;
        case PNG_PF_Up:
            prediction = pB;
            break;
        case PNG_PF_Average:
            prediction = ((pA + pB) / 2);
            break;
        case PNG_PF_Paeth:
            prediction = pA + pB - pC;
            dA = abs(prediction - pA);
            dB = abs(prediction - pB);
            dC = abs(prediction - pC);
            if (dA <= dB && dA <= dC) prediction = pA;
            else if (dB <= dC) prediction = pB;
            else prediction = pC;
            break;
        default:
            ASSERT(FALSE);
        }
    }
    ps.this_line[ps.line_x] = 0xFF & (inbyte + prediction);
}

/*
 * Calculate how many bytes of image data will appear
 * per line of the given image, accounting for the start
 * and increment of the current interlace pass.
 */

#define BPS (ps.image->bits_per_sample)
#define BMAX ((1<<BPS)-1)

size_t
new_line_size(
    IMG_INFO *image,
    int start,
    int increment)
{
    U32 pixels;
    size_t size;

    ASSERT(NULL != image);
    ASSERT(0 != increment);
    ASSERT(start < 8);

    if (image->width <= start) return 0;
    pixels = (((image->width - start) - 1) / increment) + 1;

    if (BPS < 8) {
        ASSERT(1 == image->samples_per_pixel);
        size = ((BPS * (pixels - 1)) / 8) + 1;
    } else {
        ASSERT(8 == BPS || 16 == BPS);
        size = pixels * image->samples_per_pixel * (BPS / 8);
    }
    return size;
}

static void
write_byte(
    void)
{
    U8 *temp, byte;
    /*
     * Advance pointers and handle interlacing.
     */
    if (++ps.line_x >= ps.line_size) {
        /*
         * We've now received all the bytes for a single
         * scanline. Here we write them to the tempfile,
         * unpacking 1, 2, and 4-bit values into whole bytes.
         */
        if (BPS < 8) {
            int pixel, got_bits;
            U32 start, increment;

            if (ps.image->is_interlaced) {
                start = starting_col[ps.interlace_pass];
                increment = col_increment[ps.interlace_pass];
            } else {
                start = 0;
                increment = 1;
            }
            temp = ps.this_line;
            got_bits = 0;

            for (ps.current_col = start;
              ps.current_col < ps.image->width;
              ps.current_col += increment) {

                if (got_bits == 0) {
                    byte = *temp++;
                    got_bits = 8;
                }
                pixel = (byte >> (8 - BPS)) & BMAX;
                pixel = (pixel * 255) / BMAX;

                byte <<= BPS;
                got_bits -= BPS;

                putc(pixel, ps.tf[ps.interlace_pass]);
            }
        } else {
            if (ps.line_size != fwrite(ps.this_line, 1,
              ps.line_size, ps.tf[ps.interlace_pass]))
              error_exit(ERR_WRITE);
        }
        ps.cur_filter = 255;
        ps.line_x = 0;
        temp = ps.last_line;
        ps.last_line = ps.this_line;
        ps.this_line = temp;

        if (ps.image->is_interlaced) {
            ps.current_row +=
              row_increment[ps.interlace_pass];

            if (ps.current_row >= ps.image->height) {
                /*
                 * Some odd special cases here to deal with:
                 * First, after the last pixel has been read, the
                 * pass will be incremented to 7; we decrement
                 * it back to 6 so that the calculations won't
                 * bomb. Then, we have to deal with images less
                 * than 5 pixels wide, where pass 1 will be
                 * absent--we check this because line_size will
                 * computed < 1.
                 */
                do {
                    if (++ps.interlace_pass > 6) {
                        --ps.interlace_pass;
                        return;
                    }
                    ps.current_row =
                      starting_row[ps.interlace_pass];
                    ps.line_size = new_line_size(ps.image,
                      starting_col[ps.interlace_pass],
                      col_increment[ps.interlace_pass]);
                } while (ps.line_size < 1);

                memset(ps.last_line, 0, ps.line_size);
            }
        } else {
            ++ps.current_row;
        }
    }
}

/*
 * The image has now been read into 1 or 7 temp files, at one
 * more bytes per pixel (to simplfy de-interlacing). This
 * function combines them back into a single file, pointed to
 * by the pixel_data_file member of the image structure.
 */

static int
repack_tempfiles(
    void)
{
    char filename[FILENAME_MAX];
    FILE *outf;
    U32 row, col;
    size_t bytes;
    int pass, err, byte, bpp;
    U8 *line_buf, *lp;

    ASSERT(0 != ps.buf);
    ASSERT(0 != ps.image);

    sprintf(filename, "pngdata.tmp");
    ps.image->pixel_data_file =
      (char *)malloc(strlen(filename) + 1);
    if (NULL == ps.image->pixel_data_file) return ERR_MEMORY;
    else strcpy(ps.image->pixel_data_file, filename);

    outf = fopen(ps.image->pixel_data_file, "wb");
    if (NULL == outf) return ERR_WRITE;

    if (ps.image->is_interlaced) {
        for (pass = 0; pass <= 6; ++pass) {
            if (0 != (err = open_tempfile(pass))) return err;
        }
        bpp = ps.image->samples_per_pixel;
        if (16 == ps.image->bits_per_sample) bpp *= 2;
        bytes = bpp * ps.image->width;

        if (NULL == (line_buf = (U8 *)malloc(bytes)))
          return ERR_MEMORY;

        for (row = 0; row < ps.image->height; ++row) {
            lp = line_buf;

            for (col = 0; col < ps.image->width; ++col) {
                pass = interlace_pattern[row & 7][col & 7];
                for (byte = 0; byte < bpp; ++byte) {
                    *lp++ = getc(ps.tf[pass]);
                }
            }
            ASSERT(bytes == (lp - line_buf));
            fwrite(line_buf, 1, bytes, outf);
        }
    } else {
        if (0 != (err = open_tempfile(0))) return err;
        if (NULL == (line_buf = (U8 *)malloc(IOBUF_SIZE)))
          return ERR_MEMORY;

        while (0 < (bytes =
          fread(line_buf, 1, IOBUF_SIZE, ps.tf[0]))) {
            fwrite(line_buf, 1, bytes, outf);
        }
    }
    free(line_buf);
    close_all_tempfiles();
    fclose(outf);
    remove_all_tempfiles();
    return 0;
}

#undef BPS
#undef BMAX

/*
 * Handle tEXt and zTXt chunks. The keywords listed in ptot.h
 * will be translated to equivalent TIFF tags. Others are just
 * passed on as unkown PNG chunks.
 */

#define KW_MAX 80 /* Longest possible matching keyword */

int
decode_text(
    void)
{
    int i, err;
    size_t kw_len, val_len;
    char *srcp, *dstp, **address = NULL;

    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);
    ASSERT(IS_ZTXT || IS_TEXT);

    get_chunk_data(ps.bytes_remaining);

    for (i = 0; i < N_KEYWORDS; ++i) {
        if (0 == strncmp(ps.buf, keyword_table[i], KW_MAX)) {
            address = &ps.image->keywords[i];
            break;
        }
    }
    if (NULL != address) {
        kw_len = strlen(ps.buf);
        if (IS_ZTXT) {
            if (PNG_CT_Deflate != ps.buf[kw_len + 1]) {
                err = ERR_BAD_PNG;
                goto dz_err_out;
            }
            ps.bytes_in_buf -= (kw_len + 2);
            ps.bufp = ps.buf + kw_len + 2;

            if (0 != (err = create_tempfile(0))) goto dz_err_out;
            zlib_start();
            inflate();
            zlib_end();
            if (0 != (err = open_tempfile(0))) goto dz_err_out;

            ps.buf[0] = '\0';
            ps.buf[1] = getc(ps.tf[0]);
            kw_len = 0;
            ps.bytes_in_buf = 2;
            ps.bytes_remaining = ps.inflated_chunk_size - 1;
        }
        *address = (char *)malloc((size_t)(ps.bytes_remaining +
          ps.bytes_in_buf - kw_len));

        dstp = *address;
        val_len = (size_t)(ps.bytes_in_buf - (kw_len + 1));
        srcp = ps.buf + kw_len + 1;

        while (0 != val_len) {
            memcpy(dstp, srcp, val_len);
            srcp = ps.buf;
            dstp += val_len;
            if (0 != ps.bytes_remaining) {
                if (IS_ZTXT) {
                    val_len = fread(ps.buf, 1, IOBUF_SIZE,
                      ps.tf[0]);
                    ps.bytes_remaining -= val_len;
                } else {
                    val_len = get_chunk_data(ps.bytes_remaining);
                }
            } else val_len = 0;
        }
        *dstp = '\0';
        err = 0;
    } else err = copy_unknown_chunk_data();

dz_err_out:
    close_all_tempfiles();
    return err;
}

/*
 * Copy unknown but copy-safe chunk.
 */

int
copy_unknown_chunk_data(
    void)
{
    int err;
    U8 small_buf[10];
    char fname[FILENAME_MAX];
    U32 output_crc;
    FILE *outf;

    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    if (NULL == ps.image->png_data_file) {
        sprintf(fname, "pngextra.tmp");
        ps.image->png_data_file =
          (char *)malloc(strlen(fname) + 1);
        if (NULL == ps.image->png_data_file) return ERR_MEMORY;
        strcpy(ps.image->png_data_file, fname);
    }
    if (NULL == (outf = fopen(ps.image->png_data_file, "ab")))
      return ERR_WRITE;

    BE_PUT32(small_buf, ps.bytes_remaining + ps.bytes_in_buf);
    BE_PUT32(small_buf+4, ps.current_chunk_name);
    output_crc = 0xFFFFFFFFL;
    output_crc = update_crc(output_crc, ps.buf+4, 4);

    err = ERR_WRITE;

    if (8 != (fwrite(small_buf, 1, 8, outf))) goto cu_err_out;

    if (0 != ps.bytes_in_buf) {
        output_crc = update_crc(output_crc, ps.buf,
          ps.bytes_in_buf);
        if (ps.bytes_in_buf != (U32)fwrite(ps.buf, 1,
          (size_t)(ps.bytes_in_buf), outf)) goto cu_err_out;
    }
    while (0 != ps.bytes_remaining) {
        get_chunk_data(ps.bytes_remaining);
        output_crc = update_crc(output_crc, ps.buf,
          ps.bytes_in_buf);
        if (ps.bytes_in_buf != fwrite(ps.buf, 1,
              (size_t)(ps.bytes_in_buf), outf)) goto cu_err_out;
        ps.bytes_remaining -= ps.bytes_in_buf;
    }
    BE_PUT32(small_buf, output_crc ^ 0xFFFFFFFFL);
    if (4 != (fwrite(small_buf, 1, 4, outf))) goto cu_err_out;

    err = 0;
cu_err_out:
    fclose(outf);
    return err;
}

/*
 * These next functions are required for interfacing with
 * Mark Adler's inflate.c.  fill_buf() is called by
 * NEXTBYTE when the I/O buffer is empty. It knows about
 * split IDATs and deals with them specially. These two
 * functions are used by zTXt as well.
 */

U8
fill_buf(
    void)
{
    int err;

    ASSERT(NULL != ps.buf);
    ASSERT(-1 == ps.bytes_in_buf);
    ASSERT(IS_ZTXT || IS_IDAT);

    if (0 == ps.bytes_remaining) {
        /*
         * Current IDAT is exhausted. Continue on to the next
         * one. Only IDATs can be split this way.
         */
        if (IS_ZTXT) return ERR_BAD_PNG;
        if (0 != (err = verify_chunk_crc())) return err;

        if (0 != (err = get_chunk_header())) return err;
        if (!IS_IDAT) return ERR_EARLY_EOI;
    }
    ps.bufp = ps.buf;
    ps.bytes_in_buf = (S32)fread(ps.buf, 1,
      (size_t)min(IOBUF_SIZE, ps.bytes_remaining), ps.inf);

    ps.bytes_remaining -= ps.bytes_in_buf;
    if (0 == ps.bytes_in_buf) return ERR_READ;
    ps.crc = update_crc(ps.crc, ps.buf, ps.bytes_in_buf);

    --ps.bytes_in_buf;
    return *ps.bufp++;
}

/*
 * Flush uncompressed bytes from inflate window. This function
 * is used for both IDAT and zTXt chunks.
 */

void
flush_window(
    U32 size)
{
    U8 *wp, byte;
    U32 length, sum1, sum2;
    int loopcount;

    ASSERT(NULL != ps.inflate_window);
    ASSERT(size > 0 && size <= ps.inflate_window_size);
    ASSERT(IS_ZTXT || IS_IDAT);
    /*
     * Compute Adler checksum on uncompressed data, then write.
     * We can safely delay the mod operation for 5552 bytes
     * without overflowing our 32-bit accumulators.
     */
    wp = ps.inflate_window;
    length = size;
    sum1 = ps.sum1;
    sum2 = ps.sum2;

    ASSERT(sum1 < 65521);
    ASSERT(sum2 < 65521);

    while (length > 0) {
        loopcount = (length > 5552) ? 5552 : length;
        length -= loopcount;

        do {
            sum1 += *wp++;
            sum2 += sum1;
        } while (--loopcount);

        sum1 %= 65521;
        sum2 %= 65521;
    }
    ps.sum1 = (U16)sum1;
    ps.sum2 = (U16)sum2;
    /*
     * Write uncompressed bytes to output file.
     */
    ps.inflated_chunk_size += size;
    if (IS_ZTXT) {
        fwrite(ps.inflate_window, 1, (size_t)size, ps.tf[0]);
    } else {
        wp = ps.inflate_window;
        length = size;

        do {
            byte = *wp++;

            if (255 == ps.cur_filter) {
                ps.cur_filter = byte;

                if (ps.cur_filter > 4) {
                    print_warning(WARN_FILTER);
                    ps.cur_filter = 0;
                }
            } else {
                unfilter(byte);
                write_byte();
            }
        } while (--length);
    }
}

#undef IS_ZTXT
#undef IS_TEXT
#undef IS_IDAT

