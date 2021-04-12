/*
 *  SERUtils - A command line utility for processing SER movie files
 *  Copyright (C) 2020  Giuseppe Fabio Nicotra <artix2 at gmail dot com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef __FITS_H__
#define __FITS_H__

#define FITSHeaderEnd(header) (FITSHeaderAdd(header, "END", NULL, NULL))

#include <stdlib.h>
#include <stdint.h>

typedef struct {
    size_t size;
    uint32_t count;
    unsigned char *header;
} FITSHeaderUnit;

FITSHeaderUnit *FITSCreateHeaderUnit();
void            FITSReleaseHeaderUnit(FITSHeaderUnit *hdr);
void           *FITSCreateDataUnit(void *srcdata, size_t size, size_t *unitsz);
int FITSHeaderAdd(FITSHeaderUnit *header, char *keyword, char *comment,
    char *valuefmt, ...);

#endif /* __FITS_H__ */
