/*
 * ptot.c
 *
 * Convert PNG (Portable Network Graphic) file to TIFF (Tag Image
 * File Format). Takes a filename argument on the command line.
 *
 **********
 *
 * HISTORY
 *
 * 95-03-10 Created by Lee Daniel Crocker <lee@piclab.com>
 *          http://www.piclab.com/piclab/index.html
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "ptot.h"

#define DEFINE_ENUMS
#include "errors.h"
#define DEFINE_STRINGS
#include "errors.h"

PNG_STATE ps = {0}; /* Referenced by tempfile.c, etc. */

char *keyword_table[N_KEYWORDS] = {
    "Author", "Copyright", "Software", "Source", "Title"
};

/*
 * Local definitions and statics
 */

static int decode_chunk(void);
static int decode_IHDR(void);
static int decode_PLTE(void);
static int decode_gAMA(void);
static int decode_tRNS(void);
static int decode_cHRM(void);
static int decode_pHYs(void);
static int decode_oFFs(void);
static int decode_sCAL(void);
static int skip_chunk_data(void);
static int validate_image(IMG_INFO *);

/*
 * Main for PTOT.  Get filename from command line, massage the
 * extensions as necessary, and call the read/write routines.
 */

int
main(
    int argc,
    char *argv[])
{
    int err;
    FILE *fp;
    char *cp, infname[FILENAME_MAX], outfname[FILENAME_MAX];
    IMG_INFO *image;

    image = (IMG_INFO *)malloc((size_t)IMG_SIZE);
    if (NULL == image) error_exit(ERR_MEMORY);

    if (argc < 2) error_exit(ERR_USAGE);
    strcpy(infname, argv[1]);
    strcpy(outfname, argv[1]);

    if (NULL == (cp = strrchr(outfname, '.'))) {
        strcat(infname, ".png");
    } else (*cp = '\0');

#ifdef _PNG2PPM_          /* WOK Wolfram M. Koerner */
    strcat(outfname, ".ppm");
#else
    strcat(outfname, ".tif");
#endif

    if (NULL == (fp = fopen(infname, "rb")))
      error_exit(ERR_READ);
    err = read_PNG(fp, image);
    fclose(fp);
    if (0 != err) error_exit(err);

    if (NULL == (fp = fopen(outfname, "wb")))
        error_exit(ERR_WRITE);

#ifdef _PNG2PPM_          /* WOK Wolfram M. Koerner */
    err = write_PPM(fp, image);
#else
    err = write_TIFF(fp, image);
#endif
    
    fclose(fp);

    if (0 != err) error_exit(err);
    return 0;
}

/*
 * Print warning, but continue.  A bad code should never be
 * passed here, so that causes an assertion failure and exit.
 */

void
print_warning(
    int code)
{
    ASSERT(PTOT_NMESSAGES > 0);
    ASSERT(code >= 0 && code < PTOT_NMESSAGES);

    fprintf(stderr, "WARNING: %s.\n", ptot_error_messages[code]);
    fflush(stderr);
}

/*
 * Print fatal error and exit.
 */

void
error_exit(
    int code)
{
    int msgindex;

    ASSERT(PTOT_NMESSAGES > 0);

    if (code < 0 || code >= PTOT_NMESSAGES) msgindex = 0;
    else msgindex = code;

    fprintf(stderr, "ERROR: %s.\n",
      ptot_error_messages[msgindex]);
    fflush(stderr);

    if (0 == code) exit(1);
    else exit(code);
}

void
Assert(
    char *filename,
    int lineno)
{
    fprintf(stderr, "ASSERTION FAILURE: "
      "Line %d of file \"%s\".\n", lineno, filename);
    fflush(stderr);
    exit(2);
}

/*
 * PNG-specific code begins here.
 *
 * read_PNG() reads the PNG file into the passed IMG_INFO struct.
 * Returns 0 on success.
 */

int
read_PNG(
    FILE *inf,
    IMG_INFO *image)
{
    int err;

    ASSERT(NULL != inf);
    ASSERT(NULL != image);

    memset(image, 0, IMG_SIZE);
    memset(&ps, 0, sizeof ps);

    ps.inf = inf;
    ps.image = image;
    if (NULL == (ps.buf = (U8 *)malloc(IOBUF_SIZE)))
      return ERR_MEMORY;
    /*
     * Skip signature and possible MacBinary header, and
     * verify signature. A more robust implementation might
     * search for the file signature anywhere in the first
     * 1k bytes or so, but in practice, the method shown
     * is adequate or file I/O applications.
     */
    fread(ps.buf, 1, 8, inf);
    ps.buf[8] = '\0';
    if (0 != memcmp(ps.buf, PNG_Signature, 8)) {
        fread(ps.buf, 1, 128, inf);
        ps.buf[128] = '\0';
        if (0 != memcmp(ps.buf+120, PNG_Signature, 8)) {
            err = ERR_BAD_PNG;
            goto err_out;
        }
    }

    ps.got_first_chunk = ps.got_first_idat = FALSE;
    do {
        if (0 != (err = get_chunk_header())) goto err_out;
        if (0 != (err = decode_chunk())) goto err_out;
        /*
         * IHDR must be the first chunk.
         */
        if (!ps.got_first_chunk &&
          (PNG_CN_IHDR != ps.current_chunk_name))
          print_warning(WARN_BAD_PNG);
        ps.got_first_chunk = TRUE;
        /*
         * Extra unused bytes in chunk?
         */
        if (0 != ps.bytes_remaining) {
            print_warning(WARN_EXTRA_BYTES);
            if (0 != (err = skip_chunk_data())) goto err_out;
        }
        if (0 != (err = verify_chunk_crc())) goto err_out;

    } while (PNG_CN_IEND != ps.current_chunk_name);

    if (!ps.got_first_idat) {
        err = ERR_NO_IDAT;
        goto err_out;
    }
    if (0 != (err = validate_image(image))) goto err_out;

    ASSERT(0 == ps.bytes_remaining);
    if (EOF != getc(inf)) print_warning(WARN_EXTRA_BYTES);

    err = 0;
err_out:
    ASSERT(NULL != ps.buf);
    free(ps.buf);
    return err;
}

/*
 * decode_chunk() is just a dispatcher, shunting the work of
 * decoding the incoming chunk (whose header we have just read)
 * to the appropriate handler.
 */

static int
decode_chunk(
    void)
{
    /*
     * Every case in the switch below should set err. We set it
     * here to gurantee that we hear about it if we don't.
     */
    int err = ERR_ASSERT;

    switch (ps.current_chunk_name) {

    case PNG_CN_IHDR:   err = decode_IHDR();    break;
    case PNG_CN_gAMA:   err = decode_gAMA();    break;
    case PNG_CN_IDAT:   err = decode_IDAT();    break;
    /*
     * PNG allows a suggested colormap for 24-bit images. TIFF
     * does not, and PLTE is not copy-safe, so we discard it.
     */
    case PNG_CN_PLTE:
        if (ps.image->is_palette) err = decode_PLTE();
        else err = skip_chunk_data();
        break;

    case PNG_CN_tRNS:   err = decode_tRNS();    break;
    case PNG_CN_cHRM:   err = decode_cHRM();    break;
    case PNG_CN_pHYs:   err = decode_pHYs();    break;
    case PNG_CN_oFFs:   err = decode_oFFs();    break;
    case PNG_CN_sCAL:   err = decode_sCAL();    break;

    case PNG_CN_tEXt:   err = decode_text();    break;
    case PNG_CN_zTXt:   err = decode_text();    break;

    case PNG_CN_tIME:   /* Will be recreated */
    case PNG_CN_hIST:   /* Not safe to copy */
    case PNG_CN_bKGD:
        err = skip_chunk_data();
        break;
    case PNG_CN_IEND:   /* We're done */
        err = 0;
        break;
    /*
     * Note: sBIT does not have the "copy-safe" bit set, but that
     * really only applies to unknown chunks. We know what it is
     * just like PLTE, and that it's probably safe to put in the
     * output file. hIST and bKGD are not (modifications to the
     * output file might invalidate them), so we leave them out.
     */
    case PNG_CN_sBIT:
        err = copy_unknown_chunk_data();
        break;
    default:
        if (0 == (ps.current_chunk_name & PNG_CF_CopySafe))
          err = skip_chunk_data();
        else err = copy_unknown_chunk_data();
        break;
    }
    return err;
}

/*
 * get_chunk_header() reads the first 8 bytes of each chunk, which
 * include the length and ID fields.  It returns 0 on success.
 * The crc argument is preconditioned and then updated with the
 * chunk name read.
 */

int
get_chunk_header(
    void)
{
    int byte;

    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);

    if (8 != fread(ps.buf, 1, 8, ps.inf)) return ERR_READ;

    ps.bytes_remaining = BE_GET32(ps.buf);
    ps.current_chunk_name= BE_GET32(ps.buf+4);
    ps.bytes_in_buf = 0;

    if (ps.bytes_remaining > PNG_MaxChunkLength)
      print_warning(WARN_BAD_PNG);

    for (byte = 4; byte < 8; ++byte)
      if (!isalpha(ps.buf[byte])) return ERR_BAD_PNG;

    ps.crc = update_crc(0xFFFFFFFFL, ps.buf+4, 4);
    return 0;
}

/*
 * get_chunk_data() reads chunk data into the buffer,
 * returning the number of bytes actually read.  Do not
 * use this for IDAT chunks; they are dealt with specially
 * by the fill_buf() function.
 */

U32
get_chunk_data(
    U32 bytes_requested)
{
    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);

    ps.bytes_in_buf = (U32)fread(ps.buf, 1,
      (size_t)min(IOBUF_SIZE, bytes_requested), ps.inf);

    ASSERT((S32)(ps.bytes_remaining) >= ps.bytes_in_buf);
    ps.bytes_remaining -= ps.bytes_in_buf;

    ps.crc = update_crc(ps.crc, ps.buf, ps.bytes_in_buf);
    return ps.bytes_in_buf;
}

/*
 * Assuming we have read a chunk header and all the chunk data,
 * we now check to see that the CRC stored at the end of the
 * chunk matches the one we've calculated.
 */

int
verify_chunk_crc(
    void)
{
    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);

    if (4 != fread(ps.buf, 1, 4, ps.inf)) return ERR_READ;

    if ((ps.crc ^ 0xFFFFFFFFL) != BE_GET32(ps.buf)) {
        print_warning(WARN_BAD_CRC);
    }
    return 0;
}

/*
 * Read and decode IHDR. Errors that would probably cause the
 * IDAT reader to fail are returned as errors; less serious
 * errors generate a warning but continue anyway.
 */

static int
decode_IHDR(
    void)
{
    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    if (ps.bytes_remaining < 13) return ERR_BAD_PNG;
    if (13 != get_chunk_data(13)) return ERR_READ;

    ps.image->width = BE_GET32(ps.buf);
    ps.image->height = BE_GET32(ps.buf+4);

    if (0 != ps.buf[10] || 0 != ps.buf[11])
      return ERR_BAD_PNG;   /* Compression & filter type */

    ps.image->is_interlaced = ps.buf[12];
    if (!(0 == ps.image->is_interlaced ||
      1 == ps.image->is_interlaced)) return ERR_BAD_PNG;

    ps.image->is_color = (0 != (ps.buf[9] & PNG_CB_Color));
    ps.image->is_palette = (0 != (ps.buf[9] & PNG_CB_Palette));
    ps.image->has_alpha = (0 != (ps.buf[9] & PNG_CB_Alpha));

    ps.image->samples_per_pixel = 1;
    if (ps.image->is_color && !ps.image->is_palette)
      ps.image->samples_per_pixel = 3;
    if (ps.image->has_alpha) ++ps.image->samples_per_pixel;

    if (ps.image->is_palette && ps.image->has_alpha)
      print_warning(WARN_BAD_PNG);
    /*
     * Check for invalid bit depths.  If a bitdepth is
     * not one we can read, abort processing.  If we can
     * read it, but it is illegal, issue a warning and
     * continue anyway.
     */
    ps.image->bits_per_sample = ps.buf[8];

    if (!(1 == ps.buf[8] || 2 == ps.buf[8] || 4 == ps.buf[8] ||
      8 == ps.buf[8] || 16 == ps.buf[8])) return ERR_BAD_PNG;

    if ((ps.buf[8] > 8) && ps.image->is_palette)
      print_warning(WARN_BAD_PNG);

    if ((ps.buf[8] < 8) && (2 == ps.buf[9] || 4 == ps.buf[9] ||
      6 == ps.buf[9])) return ERR_BAD_PNG;

    return 0;
}

/*
 * Decode gAMA chunk.
 */

static int
decode_gAMA(
    void)
{
    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    if (0 != ps.image->palette_size)
      print_warning(WARN_LATE_GAMA);

    if (ps.bytes_remaining < 4) return ERR_BAD_PNG;
    if (4 != get_chunk_data(4)) return ERR_READ;

    ps.image->source_gamma = (double)BE_GET32(ps.buf) / 100000.0;
    return 0;
}

/*
 * Decode PLTE chunk. Number of entries is determined by
 * chunk length. A non-multiple of 3 is technically an error;
 * we just issue a warning in that case. IOBUF_SIZE must be
 * 768 or greater, so we check that at compile time here.
 */

#if (IOBUF_SIZE < 768)
#  error "IOBUF_SIZE must be >= 768"
#endif

static int
decode_PLTE(
    void)
{
    U32 bytes_read;

    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    if (!ps.image->is_color) print_warning(WARN_PLTE_GRAY);
    if (0 != ps.image->palette_size) {
        print_warning(WARN_MULTI_PLTE);
        return skip_chunk_data();
    }
    ps.image->palette_size =
      min(256, (int)(ps.bytes_remaining / 3));
    if (0 == ps.image->palette_size) return ERR_BAD_PNG;

    bytes_read = get_chunk_data(3 * ps.image->palette_size);
    if (bytes_read < (U32)(3 * ps.image->palette_size))
      return ERR_READ;

    memcpy(ps.image->palette, ps.buf, 3 * ps.image->palette_size);

    ASSERT(0 != ps.image->palette_size);
    return 0;
}

/*
 * Copy transparency data into structure. We will later expand the
 * TIFF data into full alpha to account for its lack of this data.
 */

static int
decode_tRNS(
    void)
{
    int i;
    U32 bytes_read;

    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    if (ps.image->has_trns) print_warning(WARN_MULTI_TRNS);
    ps.image->has_trns = TRUE;

    if (ps.image->is_palette) {
        if (0 == ps.image->palette_size) {
            print_warning(WARN_LATE_TRNS);
        }
        bytes_read = get_chunk_data(ps.bytes_remaining);
        memcpy(ps.image->palette_trans_bytes,
          ps.buf, (size_t)bytes_read);

        for (i = bytes_read; i < ps.image->palette_size; ++i)
          ps.image->palette_trans_bytes[i] = 255;

    } else if (ps.image->is_color) {
        if (ps.bytes_remaining < 6) return ERR_BAD_PNG;
        bytes_read = get_chunk_data(6);
        for (i = 0; i < 3; ++i)
          ps.image->trans_values[i] = BE_GET16(ps.buf + 2 * i);
    } else {
        if (ps.bytes_remaining < 2) return ERR_BAD_PNG;
        ps.image->trans_values[0] = BE_GET16(ps.buf);
    }
    return 0;
}

static int
decode_cHRM(
    void)
{
    int i;

    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    if (ps.bytes_remaining < 32) return ERR_BAD_PNG;
    if (32 != get_chunk_data(32)) return ERR_READ;

    for (i = 0; i < 8; ++i)
      ps.image->chromaticities[i] = BE_GET32(ps.buf + 4 * i);

    return 0;
}

static int
decode_pHYs(
    void)
{
    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    if (ps.bytes_remaining < 9) return ERR_BAD_PNG;
    if (9 != get_chunk_data(9)) return ERR_READ;

    ps.image->resolution_unit = ps.buf[8];
    if (ps.buf[8] > PNG_MU_Meter) print_warning(WARN_BAD_VAL);

    ps.image->xres = BE_GET32(ps.buf);
    ps.image->yres = BE_GET32(ps.buf + 4);

    return 0;
}

static int
decode_oFFs(
    void)
{
    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    if (ps.bytes_remaining < 9) return ERR_BAD_PNG;
    if (9 != get_chunk_data(9)) return ERR_READ;

    ps.image->offset_unit = ps.buf[8];
    if (ps.buf[8] > PNG_MU_Micrometer) print_warning(WARN_BAD_VAL);

    ps.image->xoffset = BE_GET32(ps.buf);
    ps.image->yoffset = BE_GET32(ps.buf + 4);

    return 0;
}

/*
 * Decode sCAL chunk. Note: as of this writing, this is not
 * an official PNG chunk. It probably will be by the time
 * you read this, but it might possibly change in some way.
 * You have been warned. It also has no TIFF equivalent, so
 * this only gets read into the structure.
 */

static int
decode_sCAL(
    void)
{
    ASSERT(NULL != ps.inf);
    ASSERT(NULL != ps.buf);
    ASSERT(NULL != ps.image);

    get_chunk_data(ps.bytes_remaining);
    if (ps.bytes_in_buf == IOBUF_SIZE) {
        --ps.bytes_in_buf;
        print_warning(WARN_BAD_PNG);
    }
    ps.buf[ps.bytes_in_buf] = '\0';

    ps.image->scale_unit = ps.buf[0];
    if (ps.buf[0] < PNG_MU_Meter || ps.buf[0] > PNG_MU_Radian)
      print_warning(WARN_BAD_VAL);

    ps.image->xscale = atof(ps.buf+1);
    ps.image->yscale = atof(ps.buf + (strlen(ps.buf+1)) + 2);

    return 0;
}

/*
 * Skip all remaining data in current chunk.
 */

static int
skip_chunk_data(
    void)
{
    U32 bytes_read;

    do {
        bytes_read = get_chunk_data(ps.bytes_remaining);
    } while (0 != bytes_read);

    return 0;
}

/*
 * Ensure that the image structure we have created by reading
 * the input PNG is compatible with whatever we intend to do
 * with it. In this case, TIFF can handle anything, so we just
 * use this as a sanity check on some basic assumptions.
 */

static int
validate_image(
    IMG_INFO *image)
{
    if (0 == image->width || 0 == image->height)
      return ERR_BAD_IMAGE;
    if (image->samples_per_pixel < 1 ||
      image->samples_per_pixel > 4) return ERR_BAD_IMAGE;
    if (image->is_palette && (image->palette_size < 1 ||
      image->palette_size > 256)) return ERR_BAD_IMAGE;
    if (NULL == image->pixel_data_file) return ERR_BAD_IMAGE;

    return 0;
}

/*
 * End of ptot.c.
 */
