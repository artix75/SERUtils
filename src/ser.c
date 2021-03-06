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
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include "log.h"
#include "ser.h"

#define NANOSEC_PER_SEC     1000000000
#define MICROSEC_PER_SEC    1000000
#define TIMEUNITS_PER_SEC   (NANOSEC_PER_SEC / 100)
#define SECS_UNTIL_UNIXTIME 62135596800

/* Utils */

static void swapint16(void *n) {
    unsigned char *x = n, t;
    t = x[0];
    x[0] = x[1];
    x[1] = t;
}

static void swapint32(void *n) {
    unsigned char *x = n, t;
    t = x[0];
    x[0] = x[3];
    x[3] = t;
    t = x[1];
    x[1] = x[2];
    x[2] = t;
}

static void swapint64(void *n) {
    unsigned char *x = n, t;
    t = x[0];
    x[0] = x[7];
    x[7] = t;
    t = x[1];
    x[1] = x[6];
    x[6] = t;
    t = x[2];
    x[2] = x[5];
    x[5] = t;
    t = x[3];
    x[3] = x[4];
    x[4] = t;
}

static void swapMovieHeader(SERHeader *header) {
    swapint32(&header->uiLuID);
    swapint32(&header->uiColorID);
    swapint32(&header->uiLittleEndian);
    swapint32(&header->uiImageWidth);
    swapint32(&header->uiImageHeight);
    swapint32(&header->uiPixelDepth);
    swapint32(&header->uiFrameCount);
    swapint64(&header->ulDateTime);
    swapint64(&header->ulDateTime_UTC);
}

/* Helpers */

static FILE *openMovieFileForReading(SERMovie *movie, char **err) {
    if (movie->file != NULL) return movie->file;
    if (movie->filepath == NULL) {
        if (err != NULL) *err = "Missing movie filepath";
        return NULL;
    }
    FILE *video = fopen(movie->filepath, "r");
    if (video == NULL) {
        if (err != NULL) *err = "Could not open movie file for reading";
        return NULL;
    }
    movie->file = video;
    return video;
}

static int parseHeader(SERMovie *movie) {
    if (movie->header != NULL) return 1;
    movie->header = malloc(sizeof(SERHeader));
    if (movie->header == NULL) {
        fprintf(stderr, "Out-of-memory: failed to allocate movie header\n");
        return 0;
    }
    if (movie->file == NULL) {
        char *err = NULL;
        if (openMovieFileForReading(movie, &err) == NULL) {
            if (err == NULL) err = "Failed to open movie file";
            SERLogErr(LOG_TAG_ERR "%s\n", err);
            return 0;
        }
    }
    size_t hdrsize = sizeof(SERHeader);
    size_t totread = 0, nread = 0;
    char *hdrptr = (char *) movie->header;
    fseek(movie->file, 0, SEEK_SET);
    while (totread < hdrsize) {
        nread = fread((void *) hdrptr, 1, hdrsize, movie->file);
        if (nread <= 0) {
            SERLogErr(LOG_TAG_ERR "Failed to read SER movie header\n");
            return 0;
        }
        if (totread == hdrsize) break;
        totread += nread;
        hdrptr += nread;
    }
    if (IS_BIG_ENDIAN) swapMovieHeader(movie->header);
    /*printf("Read %lu header bytes\n\n", totread);*/
    return 1;
}

static uint16_t getTruncatedUInt16(uint16_t value, uint32_t pixel_size) {
    if (pixel_size >= 16) return value;
    uint32_t lshift = 16 - pixel_size,
             rshift = pixel_size - lshift;
    return (value << lshift) + (value >> rshift);
}

/* Library functions */

char *SERGetColorString(uint32_t colorID) {
    switch (colorID) {
        case COLOR_MONO: return "MONO";
        case COLOR_BAYER_RGGB: return "RGGB";
        case COLOR_BAYER_GRBG: return "GRBG";
        case COLOR_BAYER_GBRG: return "GBRG";
        case COLOR_BAYER_BGGR: return "BGGR";
        case COLOR_BAYER_CYYM: return "CYYM";
        case COLOR_BAYER_YCMY: return "YCMY";
        case COLOR_BAYER_YMCY: return "YMCY";
        case COLOR_BAYER_MYYC: return "MYYC";
        case COLOR_RGB: return "RGB";
        case COLOR_BGR: return "BGR";
    }
    return "UNKNOWN";
}

/* Convert SER datetime to Unixtime. If `usec` is not NULL, it can be used
 * to retrieve microseconds. */
time_t SERVideoTimeToUnixtime(uint64_t video_t, uint32_t *usec) {
    double elapsed_sec = video_t / (double) TIMEUNITS_PER_SEC;
    uint64_t seconds = (uint64_t) elapsed_sec - SECS_UNTIL_UNIXTIME;
    if (usec != NULL)
        *usec = (uint64_t)(elapsed_sec * MICROSEC_PER_SEC)  % MICROSEC_PER_SEC;
    return seconds;
}

/* Get the number of planes (channels) specified in the movie header.
 * "Mono" movies have one plane and RGB movies have three channels. */
int SERGetNumberOfPlanes(SERHeader *header) {
    uint32_t color = header->uiColorID;
    if (color >= COLOR_RGB) return 3;
    return 1;
}

int SERGetBytesPerPixel(SERHeader *header) {
    uint32_t depth = header->uiPixelDepth;
    if (depth < 1) return 0;
    int planes = SERGetNumberOfPlanes(header);
    if (depth <= 8) return planes;
    else return (2 * planes);
}

/* Get the number of bytes for every single frame. */
size_t SERGetFrameSize(SERHeader *header) {
    int bytes_per_px = SERGetBytesPerPixel(header);
    return header->uiImageWidth * header->uiImageHeight * bytes_per_px;
}

/* Get the offset, in bytes, of the frame `frame_idx` relative to the
 * movie file (`frame_idx` starts from zero). */
long SERGetFrameOffset(SERHeader *header, int frame_idx) {
    return sizeof(SERHeader) + (frame_idx * SERGetFrameSize(header));
}

/* Get the offset, in bytes, of tbhe movie's trailer containing frame
 * timestamps. */
long SERGetTrailerOffset(SERHeader *header) {
    uint32_t frame_idx = header->uiFrameCount;
    return SERGetFrameOffset(header, frame_idx);
}

void SERReleaseFrame(SERFrame *frame) {
    if (frame == NULL) return;
    if (frame->data != NULL) free(frame->data);
    free(frame);
}

/* Get a single frame from the movie. The returned frame is a pointer to
 * an allocated SERFrame strcuture containing both frame's metadata and
 * frame's raw data. It's up to you to release the frame by using the
 * `SERReleaseFrame` function.
 * Frame index `frame_idx` parameter starts from zero.
 * If frame is not found (ie. if `frame_idx` is beyond the number of
 * movie's frames, return NULL. */
SERFrame *SERGetFrame(SERMovie *movie, uint32_t frame_idx) {
    SERFrame *frame = NULL;
    assert(movie->header != NULL);
    if (frame_idx >= SERGetFrameCount(movie)) {
        SERLogErr(LOG_TAG_ERR "Frame index %d beyond movie frames (%d)\n",
            frame_idx, SERGetFrameCount(movie));
        return NULL;
    }
    frame = malloc(sizeof(*frame));
    if (frame == NULL) {
        SERLogErr(LOG_TAG_FATAL "Out-of-memory\n");
        return NULL;
    }
    memset(frame, 0, sizeof(*frame));
    frame->size = SERGetFrameSize(movie->header);
    size_t offset_start = sizeof(SERHeader) + (frame_idx * frame->size);
    size_t offset_end = offset_start + frame->size;
    if (movie->filesize < offset_start) {
        SERLogErr(LOG_TAG_ERR
            "Missing frame at index %d, movie frames incomplete\n",
            frame_idx
        );
        goto fail;
    } else if (movie->filesize < offset_end) {
        SERLogErr(LOG_TAG_ERR, "Incomplete data for frame %d\n", frame_idx);
        goto fail;
    }
    frame->id = frame_idx + 1;
    frame->index = frame_idx;
    frame->datetime = SERGetFrameDate(movie, frame_idx);
    if (frame->datetime > 0)
        frame->unixtime = SERVideoTimeToUnixtime(frame->datetime, NULL);
    else frame->unixtime = 0;
    frame->littleEndian = movie->header->uiLittleEndian;
    frame->pixelDepth = movie->header->uiPixelDepth;
    frame->colorID = movie->header->uiColorID;
    frame->width = movie->header->uiImageWidth;
    frame->height = movie->header->uiImageHeight;
    frame->data = malloc(frame->size);
    if (frame->data == NULL) {
        SERLogErr(LOG_TAG_FATAL "Out-of-memory\n");
        goto fail;
    }
    if (fseek(movie->file, offset_start, SEEK_SET) < 0) {
        SERLogErr(LOG_TAG_ERR, "Failed to read frame %d\n", frame_idx);
        goto fail;
    }
    size_t nread = 0, totread = 0, remain = frame->size;
    char *p = (char *) frame->data;
    while (totread < frame->size) {
        nread = fread(p, 1, remain, movie->file);
        if (nread <= 0) break;
        totread += nread;
        remain -= nread;
        p += nread;
    }
    if (totread != frame->size) {
        SERLogErr(LOG_TAG_ERR, "Failed to read frame %d\n", frame_idx);
        goto fail;
    }
    return frame;
fail:
    if (frame != NULL) SERReleaseFrame(frame);
    return NULL;
}

/* Retrieve a single pixel from a single frame. Set `big_endian` to 1
 * if you want pixel to be read in big-endian order.
 * Pixel value is stored in the mandatory SERPixelValue structure pointed by
 * the `value` argument.
 * If frame is not valid or if x/y coordinates are beyond frame's size,
 * return 0, otherwise return 1. */
int SERGetFramePixel(SERMovie *movie, SERFrame *frame, uint32_t x, uint32_t y,
    int big_endian, SERPixelValue *value)
{
    assert(value != NULL);
    if (frame->data == NULL) {
        SERLogErr("Missing data for frame %d\n", frame->id);
        return 0;
    }
    if (x >= frame->width || y >= frame->height) {
        SERLogErr("Pixel %d,%d aoutside of frame %d coordinates: %d,%d\n",
            x, y, frame->id, frame->width, frame->height
        );
        return 0;
    }
    int channels = (frame->colorID >= COLOR_RGB ? 3 : 1),
        channel_size = (frame->pixelDepth <= 8 ? 1 : 2);
    int bytes_per_px = channels * channel_size;
    uint32_t offset = (y * frame->width * bytes_per_px) + (x * bytes_per_px);
    char *data = (char *) frame->data + offset;
    /* For some reason, littleEndian = 1 if it actually is big endian.
     * For more info:
     * https://free-astro.org/index.php/SER#Specification_issue_with_endianness
     */
    int same_endianess = (big_endian == SERIsBigEndian(movie));
    int is_rgb = (frame->colorID >= COLOR_RGB);
    if (channel_size == 1) {
        /* 1-8 bit frames */
        uint8_t r, g, b;
        if (frame->colorID == COLOR_RGB) {
            r = *((uint8_t *) data++);
            g = *((uint8_t *) data++);
            b = *((uint8_t *) data++);
        } else if (frame->colorID == COLOR_BGR) {
            b = *((uint8_t *) data++);
            g = *((uint8_t *) data++);
            r = *((uint8_t *) data++);
        } else value->int8 = *((uint8_t *) data);
        if (is_rgb) {
            value->rgb8.r = r;
            value->rgb8.g = g;
            value->rgb8.b = b;
        }
    } else {
        /* 9-16 bit frames */
        uint16_t r, g, b, val;
        uint32_t lshift = 0, rshift = 0;
        if (frame->pixelDepth < 16) {
            lshift = 16 - frame->pixelDepth;
            rshift = frame->pixelDepth - lshift;
        }
        if (!is_rgb) {
            val = *((uint16_t *) data);
            if (!same_endianess) swapint16(&val);
            if (frame->pixelDepth < 16)
                val = (val << lshift) + (val >> rshift);
            value->int16 = val;
        } else {
            if (frame->colorID == COLOR_RGB) {
                r = *((uint16_t *) data++);
                g = *((uint16_t *) data++);
                b = *((uint16_t *) data++);
            } else if (frame->colorID == COLOR_BGR) {
                b = *((uint16_t *) data++);
                g = *((uint16_t *) data++);
                r = *((uint16_t *) data++);
            }
            if (!same_endianess) {
                swapint16(&r);
                swapint16(&g);
                swapint16(&b);
            }
            if (frame->pixelDepth < 16) {
                r = (r << lshift) + (r >> rshift);
                g = (g << lshift) + (g >> rshift);
                b = (b << lshift) + (b >> rshift);
            }
            value->rgb16.r = r;
            value->rgb16.g = g;
            value->rgb16.b = b;
        }
    }
    return 1;
}

/* Retrieve pixels from a single frame. Set `big_endian` to 1 if you need
 * pixels to be represented in big endian byte order (depending on your system
 * or destination storage).
 * Use the mandatory `size` pointer to retrieve frame's number of bytes.
 * The function allocates memory for pixel data. It's up to you to free
 * the allocated memory.
 * The frame index parameter (`frame_idx`) starts from zero.
 * If frame is not found (ie. if `frame_idx` is beyond the number of
 * movie's frames, return NULL. */
void *SERGetFramePixels(SERMovie *movie, uint32_t frame_idx, int big_endian,
    size_t *size)
{
    void *pixels = NULL;
    assert(size != NULL);
    *size = 0;
    SERFrame *frame = SERGetFrame(movie, frame_idx);
    if (frame == NULL) goto fail;
    *size = SERGetFrameSize(movie->header);
    if (*size == 0) goto fail;
    pixels = malloc(*size);
    if (pixels == NULL) goto fail;
    memset(pixels, 0, *size);
    int channels = SERGetNumberOfPlanes(movie->header),
        depth = (int) frame->pixelDepth,
        chsize = (depth > 8 ? 2 : 1),
        same_endianess = (big_endian == SERIsBigEndian(movie)),
        is_mono = (channels == 1);
    if (chsize == 1) {
        /* 8-bit image */
        if (is_mono) memcpy(pixels, frame->data, *size);
        else {
            uint8_t c1, c2, c3, r, g, b;
            uint8_t *read_ptr = frame->data, *write_ptr = pixels;
            size_t written = 0;
            while (written < *size) {
                c1 = *(read_ptr++);
                c2 = *(read_ptr++);
                c3 = *(read_ptr++);
                if (frame->colorID == COLOR_RGB) {
                    r = c1, g = c2, b = c3;
                } else {
                    b = c1, g = c2, r = c3;
                }
                *(write_ptr++) = r;
                *(write_ptr++) = g;
                *(write_ptr++) = b;
                written += 3;
            }
        }
    } else {
        /* 8-16 bit image */
        uint16_t *read_ptr = frame->data, *write_ptr = pixels;
        size_t written = 0;
        if (is_mono) {
            uint16_t pixel;
            while (written < *size) {
                pixel = *(read_ptr++);
                /* Swap bytes if movie endianness (declared in movie's header)
                 * differs from output endianness (depending on `big_endian`
                 * argument. */
                if (!same_endianess) swapint16(&pixel);
                if (depth < 16) pixel = getTruncatedUInt16(pixel, depth);
                *(write_ptr++) = pixel;
                written += 2;
            }
        } else {
            uint16_t c1, c2, c3, r, g, b;
            while (written < *size) {
                c1 = *(read_ptr++);
                c2 = *(read_ptr++);
                c3 = *(read_ptr++);
                /* Swap bytes if movie endianness (declared in movie's header)
                 * differs from output endianness (depending on `big_endian`
                 * argument. */
                if (!same_endianess) {
                    swapint16(&c1);
                    swapint16(&c2);
                    swapint16(&c3);
                }
                if (depth < 16) {
                    c1 = getTruncatedUInt16(c1, depth);
                    c2 = getTruncatedUInt16(c2, depth);
                    c3 = getTruncatedUInt16(c3, depth);
                }
                if (frame->colorID == COLOR_RGB) {
                    r = c1, g = c2, b = c3;
                } else {
                    b = c1, g = c2, r = c3;
                }
                *(write_ptr++) = r;
                *(write_ptr++) = g;
                *(write_ptr++) = b;
                written += (3 * 2);
            }
        }
    }
    SERReleaseFrame(frame);
    return pixels;
fail:
    *size = 0;
    if (frame != NULL) SERReleaseFrame(frame);
    if (pixels != NULL) free(pixels);
    return NULL;
}

/* Read frame's timestamp from movie's trailer (if movie has one).
 * Timestamp is represented in SER movie format, that is nanoseconds
 * since 1st January of year 1 b.c. / 100.
 * Frame index (`idx`) starts from 0.
 * Use SERVideoTimeToUnixtime to convert returned timestamp to unixtime.
 * If movie has no trailer or if the specified frame is not defined in movie's
 * trailer, return zero. */
uint64_t SERGetFrameDate(SERMovie *movie, long idx) {
    uint64_t date = 0;
    SERHeader *header = movie->header;
    if (header == NULL && !parseHeader(movie)) {
        return date;
    }
    if (idx >= header->uiFrameCount) return date;
    long offset = SERGetTrailerOffset(header);
    offset += (idx * sizeof(uint64_t));
    fseek(movie->file, offset, SEEK_SET);
    char *ptr = (char *) &date;
    size_t nread = 0;
    while (nread < sizeof(date)) {
        nread = fread(ptr, 1, sizeof(date), movie->file);
        if (nread <= 0) break;
    }
    if (IS_BIG_ENDIAN && date > 0) swapint64(&date);
    return date;
}

uint64_t SERGetFirstFrameDate(SERMovie *movie) {
    return SERGetFrameDate(movie, 0);
}

uint64_t SERGetLastFrameDate(SERMovie *movie) {
    if (movie->header == NULL && !parseHeader(movie)) return 0;
    long idx = SERGetFrameCount(movie) - 1;
    return SERGetFrameDate(movie, idx);
}

SERHeader *SERDuplicateHeader(SERHeader *srcheader) {
    SERHeader *dup = malloc(sizeof(*srcheader));
    if (dup == NULL) return NULL;
    memcpy(dup, srcheader, sizeof(*srcheader));
    return dup;
}

int SERCountMovieWarnings(int warnings) {
    size_t len = sizeof(warnings), i;
    int count = 0;
    for (i = 0; i < len; i++) {
        if (warnings & (1 << i)) count++;
    }
    return count;
}

/* Close movie->file and release everything. */
void SERCloseMovie(SERMovie *movie) {
    if (movie == NULL) return;
    if (movie->header != NULL) free(movie->header);
    if (movie->file != NULL) fclose(movie->file);
    free(movie);
}

/* Create a new SERMovie object and return a pointer to it.
 * The function will also open movie->file and parse movie's header.
 * It returns NULL if anything goes wrong.
 * It's up to you to release the returned movie object by using
 * `SERCloseMovie` function. */
SERMovie *SEROpenMovie(char *filepath) {
    if (filepath == NULL) return NULL;
    SERMovie *movie = malloc(sizeof(SERMovie));
    if (movie == NULL) {
        fprintf(stderr, "Out-of-memory\n");
        return NULL;
    }
    memset(movie, 0, sizeof(SERMovie));
    movie->filepath = filepath;
    char *err = NULL;
    movie->file = openMovieFileForReading(movie, &err);
    if (movie->file == NULL) {
        if (err == NULL) err = "failed to open movie file";
        SERLogErr(LOG_TAG_ERR "%s\n", err);
        SERCloseMovie(movie);
        return NULL;
    }
    if (!parseHeader(movie)) {
        SERLogErr(LOG_TAG_ERR "Failed to parse movie header\n");
        SERCloseMovie(movie);
        return NULL;
    }
    if (strcmp(SER_FILE_ID, movie->header->sFileID) != 0) {
        SERLogErr(LOG_TAG_ERR "File is not a SER movie file\n");
        SERCloseMovie(movie);
        return NULL;
    }
    movie->warnings = 0;
    movie->invert_endianness = 0;
    movie->duration = 0;
    fseek(movie->file, 0, SEEK_END);
    movie->filesize = ftell(movie->file);
    fseek(movie->file, 0, SEEK_SET);
    uint32_t frame_c = SERGetFrameCount(movie);
    size_t trailer_offset = SERGetTrailerOffset(movie->header),
           expected_trailer_size = (frame_c * sizeof(uint64_t)),
         trailer_size = 0;
    if (movie->filesize < trailer_offset) {
        movie->warnings |= WARN_INCOMPLETE_FRAMES;
        goto has_warns;
    } else if (!SERMovieHasTrailer(movie)) {
            movie->warnings |= WARN_MISSING_TRAILER;
    } else {
        trailer_size = movie->filesize - trailer_offset;
        if (trailer_size < expected_trailer_size)
            movie->warnings |= WARN_INCOMPLETE_TRAILER;
    }
    movie->firstFrameDate = SERGetFirstFrameDate(movie);
    movie->lastFrameDate = SERGetLastFrameDate(movie);
    if (movie->lastFrameDate > movie->firstFrameDate) {
        uint64_t duration = movie->lastFrameDate - movie->firstFrameDate;
        duration /= TIMEUNITS_PER_SEC;
        movie->duration = duration;
    } else if (!(movie->warnings & WARN_INCOMPLETE_TRAILER))
        movie->warnings |= WARN_BAD_FRAME_DATES;
has_warns:
    return movie;
}
