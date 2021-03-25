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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/errno.h>

#pragma pack(1)

#define MONO        0
#define BAYER_RGGB  8
#define BAYER_GRBG  9
#define BAYER_GBRG  10
#define BAYER_BGGR  11
#define BAYER_CYYM  16
#define BAYER_YCMY  17
#define BAYER_YMCY  18
#define BAYER_MYYC  19
#define BAYER_RGB   100
#define BAYER_BGR   101

#define WARN_INCOMPLETE_FRAMES  (1 << 0)
#define WARN_INCOMPLETE_TRAILER (1 << 1)
#define WARN_BAD_FRAME_DATES    (1 << 2)

#define ACTION_EXTRACT 1

#define NANOSEC_PER_SEC     1000000000
#define TIMEUNITS_PER_SEC   (NANOSEC_PER_SEC / 100)
#define SECS_UNTIL_UNIXTIME 62135596800

#define BUFLEN 4096
#define MAXBUF (BUFLEN - 1)

#define SER_FILE_ID "LUCAM-RECORDER"

typedef struct {
    uint32_t from;
    uint32_t to;
    uint32_t count;
} SERFrameRange;

typedef struct {
    char sFileID[14];
    uint32_t uiLuID;
    uint32_t uiColorID;
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
    long filesize;
    SERHeader *header;
    uint32_t duration;
    uint64_t firstFrameDate;
    uint64_t lastFrameDate;
    int warnings;
} SERMovie;

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int min;
    int tenth_of_min;
    int sec;
    char *observer;
    char *image_info;
} WinJUPOSInfo;

typedef struct {
    int frames_from;
    int frames_to;
    int frames_count;
    int action;
    char *output_path;
    char *output_dir;
    int log_to_json;
    int use_winjupos_filename;
} MainConfig;


/* Globals */

MainConfig conf;

/* Utils */

void memrev64(void *p) {
    unsigned char *x = p, t;

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

uint64_t intrev64(uint64_t v) {
    memrev64(&v);
    return v;
}

static int fileExists(char *path) {
    struct stat info;
    int s = stat(path, &info);
    if (s != 0 && errno == ENOENT) return 0;
    return 1;
}

static int isDirectory(char *path) {
    struct stat info;
    int s = stat(path, &info);
    if (s != 0) return 0;
    return (info.st_mode & S_IFDIR);
}

char *getColorString(uint32_t colorID) {
    switch (colorID) {
        case MONO: return "MONO";
        case BAYER_RGGB: return "RGGB";
        case BAYER_GRBG: return "GRBG";
        case BAYER_GBRG: return "GBRG";
        case BAYER_BGGR: return "BGGR";
        case BAYER_CYYM: return "CYYM";
        case BAYER_YCMY: return "YCMY";
        case BAYER_YMCY: return "YMCY";
        case BAYER_MYYC: return "MYYC";
        case BAYER_RGB: return "RGB";
        case BAYER_BGR: return "BGR";
    }
    return "UNKNOWN";
};

char *basename(char *path) {
    char *s = strrchr(path, '/');
    if (s == NULL) return path;
    else return s + 1;
}

char *dirname(char *dst, char *path) {
    char *s = strrchr(path, '/');
    if (s == NULL) dst[0] = '\0';
    else {
        size_t len = s - path;
        strncpy(dst, path, len);
    }
    return dst;
}

time_t videoTimeToUnixtime(uint64_t video_t) {
    uint64_t seconds = video_t / TIMEUNITS_PER_SEC;
    return seconds - SECS_UNTIL_UNIXTIME;
}

int makeFilepath(char *dstfilepath, char *original_path, char *dir,
    char *suffix, char *ext)
{
    char *fname = basename(original_path);
    if (strlen(fname) == 0) {
        fprintf(stderr, "makeFilepath: `original_path` is a directory\n");
        return 0;
    }
    if (dir == NULL) dir = "/tmp/";
    size_t dirlen = strlen(dir);
    int has_sep = (dirlen > 0 && dir[dirlen - 1] == '/');
    int ext_has_dot = 0;
    if (ext != NULL) {
        char *last_dot = strrchr(fname, '.');
        if (last_dot != NULL) *last_dot = '\0';
        ext_has_dot = (ext[0] == '.');
    }
    strcpy(dstfilepath, dir);
    if (!has_sep) strcat(dstfilepath, "/");
    strcat(dstfilepath, fname);
    if (suffix != NULL) strcat(dstfilepath, suffix);
    if (ext != NULL) {
        if (!ext_has_dot) strcat(dstfilepath, ".");
        strcat(dstfilepath, ext);
    }
    return 1;
}

void freeWinJUPOSInfo(WinJUPOSInfo *info) {
    if (info == NULL) return;
    if (info->observer != NULL) free(info->observer);
    if (info->image_info != NULL) free(info->image_info);
    free(info);
};

WinJUPOSInfo *getWinJUPOSInfo(char *filepath) {
    /* WinJUPOS filename spec: YYYY-mm-dd-HHMM[_T]-Observer[-ImageInfo] */
    WinJUPOSInfo *info = malloc(sizeof(*info));
    if (info == NULL) {
        fprintf(stderr, "Out-of-memory!\n");
        return NULL;
    }
    memset((void*) info, 0, sizeof(*info));
    char *filename = basename(filepath), *p, *last_p;
    char name_component[255];
    name_component[0] = '\0';
    size_t namelen = strlen(filename);
    if (namelen < 15) goto invalid;
    last_p = filename;
    /* Read year */
    p = strchr(last_p, '-');
    if (p == NULL || (p - last_p) != 4) goto invalid;
    strncpy(name_component, last_p, 4);
    name_component[4] = '\0';
    info->year = atoi(name_component);
    if (info->year <= 0) goto invalid;
    last_p = ++p;
    /* Read month */
    p = strchr(last_p, '-');
    if (p == NULL || (p - last_p) != 2) goto invalid;
    strncpy(name_component, last_p, 2);
    name_component[2] = '\0';
    info->month = atoi(name_component);
    if (info->month < 1 || info->month > 12) goto invalid;
    last_p = ++p;
    /* Read day */
    p = strchr(last_p, '-');
    if (p == NULL || (p - last_p) != 2) goto invalid;
    strncpy(name_component, last_p, 2);
    info->day = atoi(name_component);
    if (info->day < 1 || info->day > 31) goto invalid;
    last_p = ++p;
    /* Read time */
    p += 4;
    if ((p - filename) > namelen) goto invalid;
    strncpy(name_component, last_p, 2);
    info->hour = atoi(name_component);
    if (info->hour < 0 || info->hour > 23) goto invalid;
    last_p += 2;
    strncpy(name_component, last_p, 2);
    info->min = atoi(name_component);
    if (info->min < 0 || info->min > 59) goto invalid;
    last_p = p;
    info->tenth_of_min = 0;
    info->sec = 0;
    if (*p == '_' || *p == '.') {
        p += 2;
        strncpy(name_component, ++last_p, 1);
        name_component[1] = '\0';
        info->tenth_of_min = atoi(name_component);
        if (info->tenth_of_min < 0 || info->tenth_of_min > 9)
            goto invalid;
        info->sec = info->tenth_of_min * 6;
        last_p = p;
    }
    /* Read the rest */
    p = strchr(last_p, '-');
    if (p == NULL) return info;
    last_p = ++p;
    if ((p - filename) >= namelen) return info;
    /* Read observer */
    p = strchr(last_p, '-');
    if (p == NULL) p = strchr(last_p, '.');
    if (p == NULL) {
        info->observer = strdup(last_p);
        return info;
    }
    info->observer = strndup(last_p, p - last_p);
    if (*p == '.') return info;
    last_p = ++p;
    if ((p - filename) >= namelen) return info;
    /* Read image info */
    p = strchr(last_p, '.');
    if (p != NULL && p - last_p > 0) {
        info->image_info = strndup(last_p, p - last_p);
    } else if ((p - filename) < namelen) {
        info->image_info = strdup(last_p);
    }
    return info;
invalid:
    if (info != NULL) freeWinJUPOSInfo(info);
    return NULL;
}

size_t generateWinJUPOSFilename(char *dst, size_t max_size, time_t time,
    char *info, char *ext)
{
    struct tm *tm = gmtime(&time);
    int tenth_of_min = tm->tm_sec / 6;
    assert(tenth_of_min <= 9);
    size_t size = strftime(dst, max_size, "%Y-%m-%d-%H%M", tm);
    size_t remain = (max_size - size);
    if (remain < 2) return size;
    char *p = dst + size;
    size += sprintf(p, "_%d", tenth_of_min);
    remain = (max_size - size);
    if (info != NULL && strlen(info) <= remain) {
        p = dst + size;
        size += snprintf(p, remain, "-%s", info);
        remain = (max_size - size);
    }
    if (ext != NULL) {
        size_t ext_size = strlen(ext);
        int has_dot = (ext[0] == '.');
        char *dot = "";
        if (!has_dot) {
            ext_size++;
            dot = ".";
        }
        if (ext_size > remain) return size;
        p = dst + size;
        size += snprintf(p, remain, "%s%s", dot, ext);
    }
    return size;
}

size_t generateWinJUPOSMovieFilename(char *dst, size_t max_size,
    SERMovie *movie, char* ext)
{
    if (movie->warnings & WARN_BAD_FRAME_DATES) goto bad_dates;
    time_t start_t = videoTimeToUnixtime(movie->firstFrameDate);
    time_t end_t = videoTimeToUnixtime(movie->lastFrameDate);
    if (start_t <= 0 || end_t <= 0 || (end_t < start_t)) goto bad_dates;
    time_t mid_t = start_t + ((end_t - start_t) / 2);
    if (mid_t == 0) goto bad_dates;
    WinJUPOSInfo *wjinfo = getWinJUPOSInfo(movie->filepath);
    char info[BUFLEN];
    info[0] = '\0';
    char *obs = movie->header->sObserver, *image_info = NULL;
    if (obs != NULL && strchr(obs, '=') != NULL) obs = NULL;
    if (obs != NULL) {
        size_t len = strlen(obs), i;
        for (i = 0; i < len; i++) {
            if (obs[i] != ' ') break;
        }
        if (i == len) obs = NULL; /* Empty */
    }
    if (obs == NULL && wjinfo != NULL) {
        obs = wjinfo->observer;
        image_info = wjinfo->image_info;
        if (obs != NULL && strlen(obs) == 0) obs = NULL;
        if (image_info != NULL && strlen(image_info) == 0) image_info = NULL;
    }
    if (obs == NULL) obs = "UNK";
    strcpy(info, obs);
    if (image_info == NULL)
        image_info = getColorString(movie->header->uiColorID);
    strcat(info, "-");
    strcat(info, image_info);
    size_t sz = generateWinJUPOSFilename(dst, max_size, mid_t, info, ext);
    freeWinJUPOSInfo(wjinfo);
    return sz;
bad_dates:
    fprintf(stderr, "Cannot generate WinJUPOS filename: bad datetimes\n");
    *dst = '\0';
    return 0;
}

int makeMovieOutputPath(char *output_path, SERMovie *movie,
    SERFrameRange *range, char *dir)
{
    char suffix_buffer[255];
    char wjfname[BUFLEN];
    char *filepath = movie->filepath, *suffix = NULL;
    int using_wjupos = 0;
    if (conf.use_winjupos_filename) {
        if (generateWinJUPOSMovieFilename(wjfname, MAXBUF, movie, NULL) > 0) {
            using_wjupos = 1;
            filepath = wjfname;
        } else {
            fprintf(stderr, "Could not generate WinJUPOS filename\n");
            return 0;
        }
    }
    if (!using_wjupos && range != NULL) {
        sprintf(suffix_buffer, "-%d-%d", range->from + 1, range->to + 1);
        suffix = suffix_buffer;
    }
    if (dir == NULL) dir = conf.output_dir;
    if (dir == NULL) dir = "/tmp/";
    if (!makeFilepath(output_path, filepath, dir, suffix, ".ser")) {
        fprintf(stderr, "Failed to create temporary filepath\n");
        return 0;
    }
    return 1;
}


/* Program functions */

void initConfig() {
    conf.frames_from = 0;
    conf.frames_to = 0;
    conf.frames_count = 0;
    conf.action = 0;
    conf.output_path = NULL;
    conf.output_dir = NULL;
    conf.log_to_json = 0;
    conf.use_winjupos_filename = 0;
};

void printHelp(char **argv) {
    fprintf(stderr, "Usage: %s [OPTIONS] SER_MOVIE\n\n", argv[0]);
    fprintf(stderr, "OPTIONS:\n\n");
    fprintf(stderr, "   --extract FRAME_RANGE    Extract frames\n");
    fprintf(stderr, "   --json                   Log movie info to JSON\n");
    fprintf(stderr, "   -o, --output FILE        Output movie path.\n");
    fprintf(stderr, "   --winjupos-format        Use WinJUPOS spec. for "
                                                 "output filename\n");
    fprintf(stderr, "   -h, --help               Print this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "NOTES:\n\n");
    fprintf(stderr, "   * The value for FRAME_RANGE can be:\n");
    fprintf(stderr, "       <from>-<to>\n");
    fprintf(stderr, "       <from>,<count>\n");
    fprintf(stderr, "       <from>\n");
    fprintf(stderr, "     You can use negative value for <from> and <to>.\n"
        "     Example: -1 means the last frame\n\n"
    );
    fprintf(stderr,
        "   * If --output is omitted, filename will be automatically "
        "determined by using\n     original filename and frame range or, if "
        "--winjupos-format has been passed,\n     by generating a WinJUPOS "
        "compatible filename.\n     Movie will be written by default to /tmp, "
        "but if --output argument is a\n     "
        "directory, the automatically determined filename will be "
        "added to it.\n");
    fprintf(stderr, "\n");
};

int parseFrameArgument (char *arg) {
    uint32_t from = 0, to = 0, count = 0, last_n = 0;
    int is_comma = 0;
    size_t arglen = strlen(arg);
    if (arglen == 0) return 0;
    char *sep = strchr(arg, '-');
    if (sep == NULL) sep = strchr(arg, ',');
    if (sep != NULL) {
        if (sep == arg) return 0;
        if (sep == (arg + (arglen - 1))) return 0;
        is_comma = (sep[0] == ',');
        *sep = '\0';
        char *last_val = sep + 1;
        from = atoi(arg);
        if (from == 0) return 0;
        last_n = atoi(last_val);
        if (last_n == 0) return 0;
        if (!is_comma) to = last_n;
        else {
            count = last_n;
            if (count <= 0) return 0;
        }
    } else {
        from = atoi(arg);
        if (from == 0) return 0;
        to = -1;
    }
    if (from > 0) from--;
    if (to > 0) to--;
    conf.frames_from = from;
    conf.frames_to = to;
    conf.frames_count = count;
    return 1;
}

int parseOptions(int argc, char **argv) {
    int i;
    if (argc == 1) {
        printHelp(argv);
        exit(1);
    }
    for (i = 1; i < argc; i++) {
        int is_last_arg = (i == (argc - 1));
        char *arg = argv[i];
        /*if (arg[0] == '-') {
            if (*(++arg) == '-') arg++;
        } else break;*/
        if (strcmp("--extract", arg) == 0) {
            if (is_last_arg) {
                fprintf(stderr, "Missing value for --extract\n");
                exit(1);
            }
            char *frames = argv[++i];
            if (!parseFrameArgument(frames)) goto invalid_extract;
            conf.action = ACTION_EXTRACT;
        } else if (strcmp("--json", arg) == 0) {
            conf.log_to_json = 1;
        } else if (strcmp("--winjupos-format", arg) == 0) {
            conf.use_winjupos_filename = 1;
        } else if (strcmp("-o", arg) == 0 || strcmp("--output", arg) == 0) {
            if (is_last_arg) {
                fprintf(stderr, "Missing value for output\n");
                exit(1);
            }
            conf.output_path = argv[++i];
        } else if (strcmp("-h", arg) == 0 || strcmp("--help", arg) == 0) {
            printHelp(argv);
            exit(1);
        } else if (arg[0] == '-') {
            fprintf(stderr, "Invalid argument `%s`\n", arg);
            exit(1);
        }
        else break;
    }
    return i;
invalid_extract:
    fprintf(stderr, "Invalid --extract value\n");
    exit(1);
    return -1;
}

int getNumberOfPlanes(SERHeader *header) {
    uint32_t color = header->uiColorID;
    if (color >= BAYER_RGB) return 3;
    return 1;
};

int getBytesPerPixel(SERHeader *header) {
    uint32_t depth = header->uiPixelDepth;
    if (depth < 1) return 0;
    int planes = getNumberOfPlanes(header);
    if (depth <= 8) return planes;
    else return (2 * planes);
}

size_t getFrameSize(SERHeader *header) {
    int bytes_per_px = getBytesPerPixel(header);
    return header->uiImageWidth * header->uiImageHeight * bytes_per_px;
}

long getFrameOffset(SERHeader *header, int frame_idx) {
    return sizeof(SERHeader) + (frame_idx * getFrameSize(header));
}

long getTrailerOffset(SERHeader *header) {
    uint32_t frame_idx = header->uiFrameCount;
    return getFrameOffset(header, frame_idx);
}

FILE *openMovieFileForReading(SERMovie *movie, char **err) {
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

int parseHeader(SERMovie *movie) {
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
            fprintf(stderr, "%s\n", err);
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
            fprintf(stderr, "Failed to read SER movie header\n");
            return 0;
        }
        if (totread == hdrsize) break;
        totread += nread;
        hdrptr += nread;
    }
    printf("Read %lu header bytes\n", totread);
    return 1;
}


uint64_t readFrameDate(SERMovie *movie, long idx) {
    uint64_t date = 0;
    SERHeader *header = movie->header;
    if (header == NULL && !parseHeader(movie)) {
        return date;
    }
    if (idx >= header->uiFrameCount) return date;
    long offset = getTrailerOffset(header);
    offset += (idx * sizeof(uint64_t));
    fseek(movie->file, offset, SEEK_SET);
    char *ptr = (char *) &date;
    int nread = 0;
    while (nread < sizeof(date)) {
        nread = fread(ptr, 1, sizeof(date), movie->file);
        if (nread <= 0) break;
    }
    /*if (date > 0) date = intrev64(date);*/
    return date;
}

uint64_t readFirstFrameDate(SERMovie *movie) {
    return readFrameDate(movie, 0);
}

uint64_t readLastFrameDate(SERMovie *movie) {
    if (movie->header == NULL && !parseHeader(movie)) return 0;
    long idx = movie->header->uiFrameCount - 1;
    return readFrameDate(movie, idx);
}

SERHeader *duplicateHeader(SERHeader *srcheader) {
    SERHeader *dup = malloc(sizeof(*srcheader));
    if (dup == NULL) return NULL;
    memcpy(dup, srcheader, sizeof(*srcheader));
    return dup;
}

int determineFrameRange(SERHeader * header, SERFrameRange *range,
        int from, int to, int count, char **err)
{
    int tot = header->uiFrameCount;
    if (from < 0) from = tot + from;
    if (from >= tot) {
        if (err != NULL) *err = "first frame beyond movie frames";
        return 0;
    }
    if (count > 0) to = from + count;
    else if (to < 0) to = tot + to;
    if (to >= tot) {
        if (err != NULL) *err = "last frame beyond movie frames";
        return 0;
    }
    if (to < from) {
        if (err != NULL) *err = "last frame < first frame";
        return 0;
    }
    count = to - from;
    range->from = from;
    range->to = to;
    range->count = count;
    return 1;
}

void closeMovie(SERMovie *movie) {
    if (movie == NULL) return;
    if (movie->header != NULL) free(movie->header);
    if (movie->file != NULL) fclose(movie->file);
    free(movie);
}

void printMovieWarnings(int warnings) {
    if (warnings & WARN_INCOMPLETE_FRAMES)
        fprintf(stderr, "WARN: movie frames are incomplete\n");
    if (warnings & WARN_INCOMPLETE_TRAILER)
        fprintf(stderr, "WARN: movie datetimes trailer is incomplete\n");
    if (warnings & WARN_BAD_FRAME_DATES)
        fprintf(stderr, "WARN: frame datetimes order is wrong\n");
}

SERMovie *openMovie(char *filepath) {
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
        fprintf(stderr, "%s\n", err);
        closeMovie(movie);
        return NULL;
    }
    if (!parseHeader(movie)) {
        fprintf(stderr, "Failed to parse movie header\n");
        closeMovie(movie);
        return NULL;
    }
    if (strcmp(SER_FILE_ID, movie->header->sFileID) != 0) {
        fprintf(stderr, "File is not a SER movie file\n");
        closeMovie(movie);
        return NULL;
    }
    movie->warnings = 0;
    movie->duration = 0;
    fseek(movie->file, 0, SEEK_END);
    movie->filesize = ftell(movie->file);
    fseek(movie->file, 0, SEEK_SET);
    long trailer_offset = getTrailerOffset(movie->header);
    if (movie->filesize < trailer_offset) {
        movie->warnings |= WARN_INCOMPLETE_FRAMES;
        goto check_warns;
    }
    movie->firstFrameDate = readFirstFrameDate(movie);
    movie->lastFrameDate = readLastFrameDate(movie);
    if (movie->lastFrameDate > movie->firstFrameDate) {
        uint64_t duration = movie->lastFrameDate - movie->firstFrameDate;
        duration /= TIMEUNITS_PER_SEC;
        movie->duration = duration;
    } else movie->warnings |= WARN_BAD_FRAME_DATES;
check_warns:
    if (movie->warnings > 0) printMovieWarnings(movie->warnings);
    return movie;
}

int writeHeaderToVideo(FILE *video, SERHeader *header) {
    fseek(video, 0, SEEK_SET);
    size_t nwritten = 0, totwritten = 0, maxbytes = sizeof(*header);
    size_t remain = maxbytes;
    char *p = (char *) header;
    while (totwritten < maxbytes) {
        nwritten = fwrite(p, 1, remain, video);
        if (nwritten <= 0) break;
        totwritten += nwritten;
        p += totwritten;
        remain -= nwritten;
    }
    printf("Written %zu/%zu header byte(s)\n", totwritten, maxbytes);
    return (totwritten == maxbytes);
}

int appendFrameToVideo(FILE *video, SERMovie *srcmovie,
    int frame_idx, char **err)
{
    SERHeader *srcheader = srcmovie->header;
    FILE *srcvideo = srcmovie->file;
    size_t frame_sz = getFrameSize(srcheader), nread = 0, nwritten = 0,
           totread = 0, totwritten = 0;
    char *buffer = NULL;
    if (frame_sz == 0) {
        if (err != NULL) *err = "invalid frame size (0)";
        goto fail;
    }
    buffer = malloc(frame_sz);
    if (buffer == NULL) {
        if (err != NULL)
            *err = "out-of-memory (could not allocate frame bufer)";
        goto fail;
    }
    long offset = getFrameOffset(srcheader, frame_idx);
    if (fseek(srcvideo, offset, SEEK_SET) < 0) {
        if (err != NULL) *err = "frame beyond movie size, cannot read frame";
        goto fail;
    }
    char *p = buffer;
    size_t remain = frame_sz;
    while (totread < frame_sz) {
        nread = fread(p, 1, remain, srcvideo);
        if (nread <= 0) break;
        totread += nread;
        p += nread;
        remain -= nread;
    }
    if (totread != frame_sz) {
        if (err != NULL) *err = "failed to read frame";
        goto fail;
    }
    p = buffer;
    remain = frame_sz;
    while (totwritten < frame_sz) {
        nwritten = fwrite(p, 1, remain, video);
        if (nwritten <= 0) break;
        totwritten += nwritten;
        p += nwritten;
        remain -= nwritten;
    }
    if (totwritten != frame_sz) {
        if (err != NULL) *err = "failed to write frame";
        goto fail;
    }
    if (buffer != NULL) free(buffer);
    return 1;
fail:
    if (buffer != NULL) free(buffer);
    return 0;
}

int extractFramesFromVideo(SERMovie *movie, char *outputpath,
    SERFrameRange *range)
{
    char *err = NULL;
    SERHeader *new_header = NULL;
    uint64_t *datetimes_buffer = NULL;
    int i = 0;
    uint32_t from, to, count;
    from = range->from;
    to = range->to;
    count = range->count;
    SERHeader *header = movie->header;
    if (header == NULL) {
        err = "missing source movie header";
        goto fail;
    }
    new_header = duplicateHeader(header);
    if (new_header == NULL) goto fail;
    new_header->uiFrameCount = count;
    int64_t utc_diff = header->ulDateTime_UTC - header->ulDateTime;
    uint64_t first_frame_date = readFrameDate(movie, from);
    uint64_t first_frame_utc = first_frame_date;
    uint64_t last_frame_date = readFrameDate(movie, to);
    if (first_frame_date == 0) {
        err = "unable to read first frame date";
        goto fail;
    }
    if (utc_diff > 0 && utc_diff < first_frame_utc)
        first_frame_utc -= utc_diff;
    new_header->ulDateTime = first_frame_date;
    new_header->ulDateTime_UTC = first_frame_utc;
    if (outputpath == NULL) {
        char opath[BUFLEN];
        SERMovie dummy_movie = {0};
        dummy_movie.filepath = movie->filepath;
        dummy_movie.header = new_header;
        dummy_movie.firstFrameDate = first_frame_date;
        dummy_movie.lastFrameDate = last_frame_date;
        if (makeMovieOutputPath(opath, &dummy_movie, range, NULL) <= 0)
            goto fail;
        outputpath = opath;
    }
    FILE *ofile = fopen(outputpath, "w");
    if (ofile == NULL) {
        fprintf(stderr, "Failed to open %s for writing\n", outputpath);
        err = "could not open output video for writing";
        goto fail;
    }
    printf("Extracting %d frame(s): %d - %d\n", count, from + 1, to + 1);
    printf("Writing movie header\n");
    if (!writeHeaderToVideo(ofile, new_header)) {
        err = "failed to write header";
        goto fail;
    }
    long offset = getFrameOffset(header, from);
    if (fseek(movie->file, offset, SEEK_SET) < 0) {
        err = "frame offset beyond movie length";
        goto fail;
    }
    size_t trailer_size = (count * sizeof(uint64_t));
    datetimes_buffer = malloc(trailer_size);
    if (datetimes_buffer == NULL) {
        err = "out-of-memory";
        goto fail;
    }
    for (i = 0; i < count; i++) {
        printf("\rWriting frames: %d/%d                         ", i+1, count);
        fflush(stdout);
        int frame_idx = from + i;
        if (!appendFrameToVideo(ofile, movie, frame_idx, &err)) {
            printf("\n");
            fflush(stdout);
            goto fail;
        }
        uint64_t datetime = readFrameDate(movie, frame_idx);
        if (datetime == 0) {
            printf("\n");
            fflush(stdout);
            err = "invalid frame date";
            goto fail;
        }
        datetimes_buffer[i] = datetime;
    }
    printf("\n");
    fflush(stdout);
    printf("Writing frame datetimes trailer\n");
    size_t nwritten = 0, totwritten = 0, remain = trailer_size;
    char *ptr = (char *) datetimes_buffer;
    while (totwritten < trailer_size) {
        nwritten = fwrite(ptr, 1, remain, ofile);
        if (nwritten <= 0) break;
        totwritten += nwritten;
        ptr += nwritten;
        remain -= nwritten;
    }
    if (totwritten != trailer_size) {
        fprintf(stderr, "Written %zu trailer byte(s) of %zu\n",
            totwritten, trailer_size);
        err = "failed to write frame datetimes trailer";
        goto fail;
    }
    printf("New video written to:\n%s\n", outputpath);
    fflush(stdout);

    if (new_header != NULL) free(new_header);
    if (datetimes_buffer != NULL) free(datetimes_buffer);
    fclose(ofile);
    return 1;
fail:
    if (ofile != NULL) fclose(ofile);
    if (new_header != NULL) free(new_header);
    if (datetimes_buffer != NULL) free(datetimes_buffer);
    if (err != NULL) {
        fprintf(stderr, "Could not extract frames: %s (frame count: %d)\n",
            err, header->uiFrameCount);
    } else fprintf(stderr, "Could not extract frames\n");
    return 0;
}

void printMetadata(SERHeader *header) {
    char fileID[15];
    char observer[40];
    char scope[40];
    char camera[40];
    strncpy(fileID, (const char*) header->sFileID, 14);
    strncpy(observer, (const char*) header->sObserver, 40);
    strncpy(camera, (const char*) header->sInstrument, 40);
    strncpy(scope, (const char*) header->sTelescope, 40);
    time_t unix_t = videoTimeToUnixtime(header->ulDateTime);
    time_t unix_t_utc = videoTimeToUnixtime(header->ulDateTime_UTC);
    fileID[14] = '\0';
    observer[39] = '\0';
    scope[39] = '\0';
    camera[39] = '\0';
    printf("File ID: %s\n", fileID);
    printf("Little Endian: %u\n", header->uiLittleEndian);
    printf("Color: %s\n", getColorString(header->uiColorID));
    printf("Width: %d\n", header->uiImageWidth);
    printf("Height: %d\n", header->uiImageHeight);
    printf("Depth: %d\n", header->uiPixelDepth);
    printf("Frames: %d\n", header->uiFrameCount);
    printf("Observer: %s\n", observer);
    printf("Camera: %s\n", camera);
    printf("Telescope: %s\n", scope);
    printf("Datetime: %llu\n", header->ulDateTime);
    printf("Datetime (UTC): %llu\n", header->ulDateTime_UTC);
    printf("Datetime (UNIX): %ld\n", unix_t);
    printf("Timestamp: %s", ctime(&unix_t));
    printf("Timestamp (UTC): %s", ctime(&unix_t_utc));
}

void printMovieInfo(SERMovie *movie) {
    if (movie->header != NULL) printMetadata(movie->header);
    printf("First Frame Date: %llu\n", movie->firstFrameDate);
    printf("Last Frame Date: %llu\n", movie->lastFrameDate);
    if (movie->firstFrameDate > 0) {
        time_t unix_t = videoTimeToUnixtime(movie->firstFrameDate);
        printf("First Frame Timestamp: %s", ctime(&unix_t));
    }
    if (movie->firstFrameDate > 0) {
        time_t unix_t = videoTimeToUnixtime(movie->lastFrameDate);
        printf("Last Frame Timestamp: %s", ctime(&unix_t));
    }
    if (movie->duration > 0)
        printf("Duration: %d sec.\n", movie->duration);
    printf("Filesize: %ld\n", movie->filesize);
}

int logToJSON(FILE *json_file, SERMovie *movie)
{
    char fileID[15];
    char observer[40];
    char scope[40];
    char camera[40];
    SERHeader *header = movie->header;
    strncpy(fileID, (const char*) header->sFileID, 14);
    strncpy(observer, (const char*) header->sObserver, 40);
    strncpy(camera, (const char*) header->sInstrument, 40);
    strncpy(scope, (const char*) header->sTelescope, 40);
    fileID[14] = '\0';
    observer[39] = '\0';
    scope[39] = '\0';
    camera[39] = '\0';
    fprintf(json_file, "{\n");
    fprintf(json_file, "    \"fileID\": \"%s\",\n", fileID);
    fprintf(json_file, "    \"littleEndian\": %u,\n",
        header->uiLittleEndian);
    fprintf(json_file, "    \"color\": \"%s\",\n",
        getColorString(header->uiColorID));
    fprintf(json_file, "    \"width\": %u,\n", header->uiImageWidth);
    fprintf(json_file, "    \"height\": %u,\n", header->uiImageHeight);
    fprintf(json_file, "    \"depth\": %u,\n", header->uiPixelDepth);
    fprintf(json_file, "    \"frames\": %u,\n", header->uiFrameCount);
    fprintf(json_file, "    \"observer\": \"%s\",\n", observer);
    fprintf(json_file, "    \"camera\": \"%s\",\n", camera);
    fprintf(json_file, "    \"telescope\": \"%s\",\n", scope);
    fprintf(json_file, "    \"datetime\": %llu,\n", header->ulDateTime);
    fprintf(json_file, "    \"datetimeUTC\": %llu,\n", header->ulDateTime_UTC);
    fprintf(json_file, "    \"firstFrameDatetime\": %llu,\n",
        movie->firstFrameDate);
    fprintf(json_file, "    \"lastFrameDatetime\": %llu\n",
        movie->lastFrameDate);
    fprintf(json_file, "}\n");
    return 1;
}

int main(int argc, char **argv) {
    initConfig();
    int filepath_idx = parseOptions(argc, argv);
    if (filepath_idx >= argc) {
        printHelp(argv);
        return 1;
    }
    char *filepath = argv[filepath_idx];
    if (conf.output_path != NULL && isDirectory(conf.output_path)) {
        conf.output_dir = conf.output_path;
        conf.output_path = NULL;
    }
    SERMovie *movie = openMovie(filepath);
    if (movie == NULL) {
        fprintf(stderr, "Could not open movie at: '%s'\n", filepath);
        goto err;
    }
    printMovieInfo(movie);
    SERHeader *header = movie->header;
    if (conf.action == ACTION_EXTRACT) {
        int from = conf.frames_from, to = conf.frames_to,
            count = conf.frames_count;
        SERFrameRange range;
        char *errmsg = NULL;
        if (!determineFrameRange(header, &range, from, to, count, &errmsg)) {
            if (errmsg == NULL) errmsg = "invalid frame range";
            fprintf(stderr, "%s\n", errmsg);
            goto err;
        }
        char *output_path = conf.output_path;
        if (conf.use_winjupos_filename) output_path = NULL;
        /*if (output_path == NULL) {
            char opath[1024];
            char suffix[255];
            sprintf(suffix, "-%d-%d", range.from + 1, range.to + 1);
            output_path = opath;
            if (!makeFilepath(output_path, filepath, "/tmp/", suffix, ".ser")) {
                fprintf(stderr, "Failed to create temporary filepath\n");
                goto err;
            }
        }*/
        int ok = extractFramesFromVideo(movie, output_path, &range);
        if (!ok) goto err;
        closeMovie(movie);
        return 0;
    }
    if (conf.log_to_json) {
        char json_filename[1024];
        makeFilepath(json_filename, filepath, "/tmp/", NULL, ".json");
        FILE *json = fopen(json_filename, "w");
        if (json == NULL) {
            fprintf(stderr, "Could not open '%s' for writing!\n",
                json_filename);
            return 1;
        }
        logToJSON(json, movie);
        printf("JSON saved to: '%s'\n", json_filename);
    }
    closeMovie(movie);
    return 0;
err:
    closeMovie(movie);
    return 1;
}