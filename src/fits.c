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

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include "log.h"
#include "fits.h"

#define FITS_HDU_MIN_SIZE       2880
#define FITS_KEYWORD_LINE_SIZE  80
#define FITS_KEYWORD_MAX_LEN    8

static int isEmptyString(char *str, size_t len) {
    if (str == NULL) return 1;
    if (len == 0) len = strlen(str);
    if (len == 0) return 1;
    size_t i;
    for (i = 0; i < len; i++) {
        char c = str[i];
        if (c != ' ' && c != '\t') return 0;
    }
    return 1;
}

static int isValidFITSKeyword(char *str, size_t len, char *invalid) {
    size_t i;
    for (i = 0; i < len; i++) {
        char c = str[i];
        if (c == '-') continue;
        int is_num = (c >= '0' && c <= '9');
        if (is_num) continue;
        if (c < 'A' || c > 'Z') {
            if (invalid != NULL) *invalid = c;
            return 0;
        }
    }
    return 1;
}

static int makeRoomForFITSHeader(FITSHeaderUnit *hdr, int count) {
    if (count <= 0) return 1;
    size_t new_size = (count * FITS_KEYWORD_LINE_SIZE);
    if (new_size <= hdr->size) return 1;
    size_t units = new_size / FITS_HDU_MIN_SIZE;
    if ((new_size % FITS_HDU_MIN_SIZE) > 0) units++;
    new_size = (units * FITS_HDU_MIN_SIZE);
    if (new_size <= hdr->size) return 1;
    hdr->header = realloc(hdr->header, new_size);
    if (hdr->header == NULL) {
        SERLogErr(LOG_TAG_FATAL "Out-of-memory\n");
        return 0;
    }
    size_t added = new_size - hdr->size;
    if (added > 0) {
        unsigned char *ptr = hdr->header + hdr->size;
        memset(ptr, ' ', added);
    }
    hdr->size = new_size;
    return 1;
}

FITSHeaderUnit *FITSCreateHeaderUnit() {
    FITSHeaderUnit *hdr = malloc(sizeof(*hdr));
    hdr->header = NULL;
    hdr->size = 0;
    hdr->count = 0;
    if (hdr == NULL) {
        SERLogErr(LOG_TAG_FATAL "Out-of-memory\n");
        return NULL;
    }
    hdr->header = malloc(FITS_HDU_MIN_SIZE);
    if (hdr->header == NULL) {
        SERLogErr(LOG_TAG_FATAL "Out-of-memory\n");
        goto fail;
    }
    hdr->size = FITS_HDU_MIN_SIZE;
    hdr->count = 0;
    memset(hdr->header, ' ', hdr->size);
    return hdr;
fail:
    if (hdr != NULL) FITSReleaseHeaderUnit(hdr);
    return NULL;
}

void FITSReleaseHeaderUnit(FITSHeaderUnit *hdr) {
    if (hdr == NULL) return;
    if (hdr->header != NULL) free(hdr->header);
    free(hdr);
}

int FITSHeaderAdd(FITSHeaderUnit *header, char *keyword, char *comment,
    char *valuefmt, ...)
{
    assert(header != NULL);
    if (keyword == NULL) {
        fprintf(stderr, "FITSHeaderAdd: keyword required\n");
        return 0;
    }
    size_t kwlen, vlen = 0, clen = 0, totlen = 0, oplen = 0, i,
           max_vlen = 0, max_clen = 0, equals_len = 1, slash_len = 3,
           avail_len = FITS_KEYWORD_LINE_SIZE - FITS_KEYWORD_MAX_LEN;
    char value[255];
    char invalid_c = 0;
    kwlen = strlen(keyword);
    if (kwlen == 0 || isEmptyString(keyword, kwlen)) {
        fprintf(stderr, "FITSHeaderAdd: keyword is empty\n");
        return 0;
    } else if (!isValidFITSKeyword(keyword, kwlen, &invalid_c)) {
        fprintf(stderr, "Invalid FITS keyword '%s': keyword must contain only "
            "uppercase characters (A-Z) or digits (0-9). Invalid char: "
            "'%c'\n", keyword, invalid_c);
        return 0;
    } else if (kwlen > FITS_KEYWORD_MAX_LEN) {
        fprintf(stderr, "WARN: keyword '%s' length %zu > %d, truncating...\n",
            keyword, kwlen, FITS_KEYWORD_MAX_LEN);
        kwlen = FITS_KEYWORD_MAX_LEN;
    }
    size_t count = header->count + 1;
    if (!makeRoomForFITSHeader(header, count)) return 0;
    unsigned char *hdrptr =
        header->header + (header->count * FITS_KEYWORD_LINE_SIZE);
    unsigned char *linestart = hdrptr;
    if (valuefmt != NULL) {
        va_list ap;
        va_start(ap, valuefmt);
        vlen = vsnprintf(value, 255, valuefmt, ap);
        va_end(ap);
        if (vlen > 0) oplen += equals_len; /* sizeof "=" */
    }
    if (comment != NULL) {
        clen = strlen(comment);
        if (isEmptyString(comment, clen)) {
            comment = NULL;
            clen = 0;
        }
        if (clen > 0) oplen += slash_len; /* sizeof " / " */
    }
    if (vlen > 0 && clen > 0) {
        max_clen = 40 - slash_len;
        max_vlen = (avail_len - equals_len - slash_len - max_clen);
    }
    else if (vlen > 0) max_vlen = (avail_len - equals_len);
    else if (clen > 0)  max_clen = (avail_len - slash_len);
    if (max_vlen > 0 && vlen > max_vlen) {
        fprintf(stderr, "WARN: value length %zu exceeds max, truncating...\n",
            vlen);
        vlen = max_vlen;
    }
    if (max_clen > 0 && clen > max_clen) {
        fprintf(stderr, "WARN: comment length %zu exceeds max, truncating...\n",
            clen);
        clen = max_clen;
    }
    totlen = kwlen + clen + vlen + oplen;
    assert(totlen <= FITS_KEYWORD_LINE_SIZE);
    memcpy(hdrptr, keyword,  kwlen);
    hdrptr += kwlen;
    size_t padlen;
    if (vlen > 0) {
        padlen = FITS_KEYWORD_MAX_LEN - kwlen;
        for (i = 0; i < padlen; i++) *(hdrptr++) = ' ';
        *(hdrptr++) = '=';
        padlen = max_vlen - vlen;
        for (i = 0; i < padlen; i++) *(hdrptr++) = ' ';
         memcpy(hdrptr, value, vlen);
         hdrptr += vlen;
    }
    if (clen > 0 && comment != NULL) {
        assert((hdrptr - linestart) <= FITS_KEYWORD_LINE_SIZE);
        memcpy(hdrptr, " / ", slash_len);
        hdrptr += slash_len;
        memcpy(hdrptr, comment, clen);
        hdrptr += clen;
    }
    padlen = (FITS_KEYWORD_LINE_SIZE - (hdrptr - linestart));
    for (i = 0; i < padlen; i++) *(hdrptr++) = ' ';
    header->count = count;
    return 1;
}

void *FITSCreateDataUnit(void *srcdata, size_t size, size_t *unit_size) {
    if (srcdata == NULL || size == 0) return NULL;
    size_t units = (size / FITS_HDU_MIN_SIZE);
    if ((size % FITS_HDU_MIN_SIZE) > 0) units++;
    size_t totsize = units * FITS_HDU_MIN_SIZE;
    void *data = malloc(totsize);
    *unit_size = 0;
    if (data == NULL) {
        SERLogErr(LOG_TAG_FATAL "Out-of-memory\n");
        return NULL;
    }
    *unit_size = totsize;
    memcpy(data, srcdata, size);
    char *fillptr = (char *)data + size;
    size_t fillsize = totsize - size;
    memset(fillptr, 0, fillsize);
    return data;
}
