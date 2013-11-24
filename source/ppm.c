/* by Wolfram M. Koerner
 Wuerzburg, Germany, 21.10.1997
 koerner@bigfoot.com, w.koerner@usa.net
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ptot.h"
#define DEFINE_ENUMS
#include "errors.h"

void kill_temp_files(IMG_INFO *image);

int
write_PPM(
    FILE *outf,
    IMG_INFO *image)
{
    int err;
    FILE* inf;
    char c;

    ASSERT(NULL != outf);
    ASSERT(NULL != image);
    ASSERT(NULL != image->pixel_data_file);

    inf = fopen(image->pixel_data_file, "rb");
    if (! inf)
        return ERR_READ;

    if (image->is_palette)                                 /* PALETTE */
    {
        printf("Sorry, no support for PALETTE PNG -> PPM/PGM\n");
        printf("Truecolor 24-Bit PNG only!\n");
        fclose(inf);
        kill_temp_files(image);
        return ERR_BAD_PNG;
    }
    
    if (!image->is_color)                                 /* GRAY */
    {
        printf("Sorry, no support for GRAY PNG -> PPM/PGM\n");
        printf("Truecolor 24-Bit PNG only!\n");
        fclose(inf);
        kill_temp_files(image);
        return ERR_BAD_PNG;
    }

    
                                                          /* RGB TRUECOLOR */
    fprintf(outf , "P6\n%d %d\n255\n", image->width, image->height);
    while (! feof(inf))
    {
        c = getc(inf);
        fwrite ((const void *)&c, 1, 1, outf);
    }

    fclose(inf);
    kill_temp_files(image);
    return 0;
}


void kill_temp_files(IMG_INFO *image)
{
    remove(image->pixel_data_file);

    if (NULL != image->png_data_size)
        remove(image->png_data_file);
}
