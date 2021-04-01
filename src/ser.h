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

#ifndef __SER_H__
#define __SER_H__

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x100)
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
    #define IS_UNIX 1
    #include <sys/ioctl.h>
    #include <unistd.h>
#else
    #define IS_UNIX 0
#endif

/* Monochromatic (one channel) formats */
#define COLOR_MONO          0
#define COLOR_BAYER_RGGB    8
#define COLOR_BAYER_GRBG    9
#define COLOR_BAYER_GBRG    10
#define COLOR_BAYER_BGGR    11
#define COLOR_BAYER_CYYM    16
#define COLOR_BAYER_YCMY    17
#define COLOR_BAYER_YMCY    18
#define COLOR_BAYER_MYYC    19
/* Color (three channels) formats */
#define COLOR_RGB           100
#define COLOR_BGR          101

#define WARN_FILESIZE_MISMATCH  (1 << 0)
#define WARN_INCOMPLETE_FRAMES  (1 << 1)
#define WARN_MISSING_TRAILER    (1 << 2)
#define WARN_INCOMPLETE_TRAILER (1 << 3)
#define WARN_BAD_FRAME_DATES    (1 << 4)

#define WARN_INCOMPLETE_FRAMES_MSG  "incomplete movie frames"
#define WARN_INCOMPLETE_TRAILER_MSG "incomplete frame dates"
#define WARN_MISSING_TRAILER_MSG    "missing frame dates"
#define WARN_BAD_FRAME_DATES_MSG    "frame dates order is wrong"
#define WARN_FILESIZE_MISMATCH_MSG  \
    "movie file size does not match header data"

#define SER_FILE_ID "LUCAM-RECORDER"

#define SERMovieHasTrailer(movie) \
    (movie->filesize > (size_t) SERGetTrailerOffset(movie->header))
#define SERGetFrameCount(movie) \
    (movie->header->uiFrameCount)
#define SERGetLastFrameIndex(movie) \
    (SERGetFrameCount(movie) - 1)

#pragma pack(1)

typedef struct {
    char sFileID[14];
    uint32_t uiLuID;
    uint32_t uiColorID;
    /* WARN: For some reason, uiLittleEndian is used in the opposite meanning,
     * so that the image data byte order is big-endian when uiLittleEndian is
     * 1, and little-endian when uiLittleEndian is 0.
     * For more info, see:
     * https://free-astro.org/index.php/SER#Specification_issue_with_endianness
     */
    uint32_t uiLittleEndian;
    uint32_t uiImageWidth;
    uint32_t uiImageHeight;
    uint32_t uiPixelDepth;
    uint32_t uiFrameCount;
    char sObserver[40];
    char sInstrument[40];
    char sTelescope[40];
    uint64_t ulDateTime;
    uint64_t ulDateTime_UTC;
} SERHeader;

typedef struct {
    char *filepath;
    FILE *file;
    size_t filesize;
    SERHeader *header;
    uint32_t duration;
    uint64_t firstFrameDate;
    uint64_t lastFrameDate;
    int warnings;
} SERMovie;

typedef union {
    uint8_t     int8;
    uint16_t    int16;
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    } rgb8;
    struct {
        uint16_t r;
        uint16_t g;
        uint16_t b;
    } rgb16;
} SERPixelValue;

typedef struct {
    uint32_t id;
    uint32_t index;
    uint64_t datetime;
    time_t unixtime;
    uint32_t littleEndian;
    uint32_t pixelDepth;
    uint32_t colorID;
    uint32_t width;
    uint32_t height;
    size_t size;
    void *data;
} SERFrame;

SERMovie   *SEROpenMovie(char *filepath);
void        SERCloseMovie(SERMovie *movie);
uint64_t    SERGetFrameDate(SERMovie *movie, long idx);
uint64_t    SERGetFirstFrameDate(SERMovie *movie);
uint64_t    SERGetLastFrameDate(SERMovie *movie);
int         SERGetNumberOfPlanes(SERHeader *header);
int         SERGetBytesPerPixel(SERHeader *header);
size_t      SERGetFrameSize(SERHeader *header);
long        SERGetFrameOffset(SERHeader *header, int frame_idx);
long        SERGetTrailerOffset(SERHeader *header);
SERFrame   *SERGetFrame(SERMovie *movie, uint32_t frame_idx);
int         SERGetFramePixel(SERFrame *frame, uint32_t x, uint32_t y,
                             SERPixelValue *value);
void       *SERGetFramePixels(SERMovie *movie, uint32_t frame_idx, size_t *sz);
void        SERReleaseFrame(SERFrame *frame);
SERHeader  *SERDuplicateHeader(SERHeader *srcheader);
int         SERCountMovieWarnings(int warnings);
char       *SERGetColorString(uint32_t colorID);
time_t      SERVideoTimeToUnixtime(uint64_t video_t);

#endif /* __SER_H__ */
