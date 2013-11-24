/*
 * tempfile.c
 *
 * Temporary file handline for ptot.
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

int
create_tempfile(
    int pass)
{
    char temp_name[FILENAME_MAX];

    ASSERT(pass >= 0 && pass < 7);
    ASSERT(NULL == ps.tf[pass]);

    if (NULL == ps.tfnames[pass]) {
        sprintf(temp_name, "pngpass%d.tmp", pass);
        ps.tfnames[pass] = (char *)malloc(strlen(temp_name) + 1);
        if (NULL == ps.tfnames[pass]) return ERR_MEMORY;
        strcpy(ps.tfnames[pass], temp_name);
    }
    ps.tf[pass] = fopen(ps.tfnames[pass], "wb");
    if (NULL == ps.tf[pass]) return ERR_WRITE;

    return 0;
}

int
open_tempfile(
    int pass)
{
    ASSERT(pass >= 0 && pass < 7);
    ASSERT(NULL != ps.tfnames[pass]);

    if (NULL != ps.tf[pass]) fclose(ps.tf[pass]);
    ps.tf[pass] = fopen(ps.tfnames[pass], "rb");
    if (NULL == ps.tf[pass]) return ERR_READ;

    return 0;
}

void
close_all_tempfiles(
    void)
{
    int pass;

    for (pass = 0; pass < 7; ++pass) {
        if (NULL != ps.tf[pass]) fclose(ps.tf[pass]);
        ps.tf[pass] = NULL;
    }
}

void
remove_all_tempfiles(
    void)
{
    int pass;

    for (pass = 0; pass < 7; ++pass) {
        if (NULL != ps.tf[pass]) fclose(ps.tf[pass]);
        if (NULL != ps.tfnames[pass]) remove(ps.tfnames[pass]);
    }
}

