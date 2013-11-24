/*
 * inflate.h
 *
 * Definitions needed for interfacing ptot to Mark Adler's 
 * inflate.c from the Info-ZIP distribution.  Most of the
 * real work is moved to ptot.h, because ptot.c needs to
 * share the structures.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ptot.h"

extern PNG_STATE ps;
