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
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/errno.h>

#pragma pack(1)

#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x100)
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
    #define IS_UNIX 1
    #include <sys/ioctl.h>
    #include <stdio.h>
    #include <unistd.h>
#else
    #define IS_UNIX 0
#endif

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

#define ACTION_NONE     0
#define ACTION_EXTRACT  1
#define ACTION_CUT      2
#define ACTION_SPLIT    3

#define SPLIT_MODE_COUNT    1
#define SPLIT_MODE_FRAMES   2
#define SPLIT_MODE_SECS     3

#define MAX_SPLIT_COUNT             50
#define MIN_SPLIT_FRAMES_PER_CHUNCK 100

#define NANOSEC_PER_SEC     1000000000
#define TIMEUNITS_PER_SEC   (NANOSEC_PER_SEC / 100)
#define SECS_UNTIL_UNIXTIME 62135596800

#define BUFLEN 4096
#define MAXBUF (BUFLEN - 1)

#define SIZE_KB 1024
#define SIZE_MB (SIZE_KB * 1024)
#define SIZE_GB (SIZE_MB * 1024)

#define SER_FILE_ID "LUCAM-RECORDER"

#define LOG_LEVEL_INFO          0
#define LOG_LEVEL_NOTICE        1
#define LOG_LEVEL_SUCCESS       2
#define LOG_LEVEL_WARN          3
#define LOG_LEVEL_ERR           4

#define LOG_TAG_ERR         "ERROR: "
#define LOG_TAG_WARN        "WARN: "
#define LOG_TAG_FATAL       "FATAL: "

#define LOG_MAX_HDR_LEN     30
#define LOG_MAX_FIELD_LEN   22
#define LOG_JUSTIFY_RIGHT   1
#define LOG_JUSTIFY_LEFT    2
#define LOG_JUSTIFY_FIELD   LOG_JUSTIFY_RIGHT

#define LOG_COLOR_RED       31
#define LOG_COLOR_GREEN     32
#define LOG_COLOR_YELLOW    33
#define LOG_COLOR_BLUE      34
#define LOG_COLOR_MAGENTA   35
#define LOG_COLOR_CYAN      36
#define LOG_COLOR_GRAY      37
#define LOG_COLOR_DEFAULT   39 /* Default foreground color */

#define getRangeCount(r) (1 + (r->to - r->from));
#define updateRangeCount(r) do {\
    assert(r->to >= r->from);\
    r->count = getRangeCount(r);\
} while (0);

#define logInfo(...) log(LOG_LEVEL_INFO, __VA_ARGS__)
#define logNotice(...) log(LOG_LEVEL_NOTICE, __VA_ARGS__)
#define logSuccess(...) log(LOG_LEVEL_SUCCESS, __VA_ARGS__)
#define logWarn(...) log(LOG_LEVEL_WARN, __VA_ARGS__)
#define logErr(...) log(LOG_LEVEL_ERR, __VA_ARGS__)


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
    int split_amount;
    int split_mode;
    int action;
    char *output_path;
    char *output_dir;
    int log_to_json;
    int use_winjupos_filename;
    int do_check;
    int overwrite;
    int use_colors;
} MainConfig;


/* Globals */

MainConfig conf;
SERFrameRange splitRanges[MAX_SPLIT_COUNT] = {0};
int split_count = 0;
char output_movie_path[MAXBUF] = {0};


/* Forward declarations */

int countMovieWarnings(int warnings);
void printMovieWarnings(int warnings);
uint64_t readFrameDate(SERMovie *movie, long idx);
static void log(int level, const char* format, ...);

/* Utils */

void swapint32(void *n) {
    unsigned char *x = n, t;
    t = x[0];
    x[0] = x[3];
    x[3] = t;
    t = x[1];
    x[1] = x[2];
    x[2] = t;
}

void swapint64(void *n) {
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

static int get_terminal_columns() {
    static int __term_columns = -1;
    if (__term_columns < 0) {
#if IS_UNIX
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        __term_columns = w.ws_col;
#else
        __term_columns = 0;
#endif
        if (__term_columns <= 0) __term_columns = 80;
    }
    return __term_columns;
}

static void removenl(char *str) {
    char *p = strrchr(str, '\n');
    if (p != NULL) *p = '\0';
}

static char *stripped_ctime(const time_t *time) {
    char *s = ctime(time);
    removenl(s);
    return s;
}

static size_t getElapsedTimeStr(char *dest, size_t max_size, time_t sec) {
    assert(max_size >= 9);
    size_t len = 0, remain = max_size;
    if (sec < 60) {
        dest[0] = '\0';
        return 0;
    }
    char *p = dest;
    time_t hour, min;
    hour = sec / 3600;
    if (hour > 0) {
        len += snprintf(p, max_size, "%02ld:", hour);
        sec %= 3600;
        remain -= len;
        p += len;
        *p = '\0';
    }
    min = sec / 60;
    len += snprintf(p, max_size, "%02ld:", min);
    sec %= 60;
    remain -= len;
    p = dest + len;
    *p = '\0';
    len += snprintf(p, max_size, "%02ld", sec);
    p = dest + len;
    *p = '\0';
    return len;
}

static size_t getFilesizeStr(char *dest, size_t max_size, long bytes) {
    size_t len = 0;
    *dest = '\0';
    float fsize = 0;
    char *unit = NULL;
    if (bytes >= SIZE_GB) {
        fsize = bytes / (float) SIZE_GB;
        unit = "GB";
    } else if (bytes >= SIZE_MB) {
        fsize = bytes / (float) SIZE_MB;
        unit = "GB";
    } else {
        fsize = bytes / (float) SIZE_KB;
        unit = "KB";
    }
    len = snprintf(dest, max_size, "%.2f%s", fsize, unit);
    return len;
}

time_t videoTimeToUnixtime(uint64_t video_t) {
    uint64_t seconds = video_t / TIMEUNITS_PER_SEC;
    return seconds - SECS_UNTIL_UNIXTIME;
}

time_t getFrameRangeDuration(SERMovie *movie, SERFrameRange *range) {
    if (range->to < range->from) return -1;
    if (range->to == range->from) return 0;
    uint64_t start_date = readFrameDate(movie, range->from),
             end_date = readFrameDate(movie, range->to);
    if (start_date == 0 || end_date == 0) return -1;
    if (end_date < start_date) return -1;
    if (start_date == end_date) return 0;
    time_t start_t = videoTimeToUnixtime(start_date),
           end_t = videoTimeToUnixtime(end_date);
    return end_t - start_t;
}

int makeFilepath(char *dstfilepath, char *original_path, char *dir,
    char *suffix, char *ext)
{
    char *fname = basename(original_path);
    if (strlen(fname) == 0) {
        logErr(LOG_TAG_FATAL "makeFilepath: `original_path` is a directory\n");
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
        logErr(LOG_TAG_FATAL "Out-of-memory!\n");
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
    logErr(LOG_TAG_ERR "Cannot generate WinJUPOS filename: bad datetimes\n");
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
            logErr("Could not generate WinJUPOS filename\n");
            return 0;
        }
    }
    if (!using_wjupos && range != NULL) {
        char *fmt = (conf.action == ACTION_CUT ? "-%d-%d-cut" : "-%d-%d");
        sprintf(suffix_buffer, fmt, range->from + 1, range->to + 1);
        suffix = suffix_buffer;
    }
    if (dir == NULL) dir = conf.output_dir;
    if (dir == NULL) dir = "/tmp/";
    if (!makeFilepath(output_path, filepath, dir, suffix, ".ser")) {
        logErr("Failed to create temporary filepath\n");
        return 0;
    }
    return 1;
}


/* Program functions */

void initConfig() {
    conf.frames_from = 0;
    conf.frames_to = 0;
    conf.frames_count = 0;
    conf.split_amount = 0;
    conf.split_mode = 0;
    conf.action = ACTION_NONE;
    conf.output_path = NULL;
    conf.output_dir = NULL;
    conf.log_to_json = 0;
    conf.use_winjupos_filename = 0;
    conf.do_check = 0;
    conf.overwrite = 0;
    conf.use_colors = 1;
};

static void log(int level, const char* format, ...) {
    FILE *out = (level > LOG_LEVEL_SUCCESS ? stderr : stdout);
    int color = 0;
    if (conf.use_colors) {
        switch (level) {
        case LOG_LEVEL_NOTICE:
            color = LOG_COLOR_CYAN; break;
        case LOG_LEVEL_SUCCESS:
            color = LOG_COLOR_GREEN; break;
        case LOG_LEVEL_WARN:
            color = LOG_COLOR_YELLOW; break;
        case LOG_LEVEL_ERR:
            color = LOG_COLOR_RED; break;
        }
    }
    if (color > 0) fprintf(out, "\033[%dm", color);
    va_list ap;
    va_start(ap, format);
    vfprintf(out, format, ap);
    va_end(ap);
    if (color > 0) fprintf(out, "\033[0m");
}

static void printHeader(char *str) {
    size_t len = strlen(str), max_len = get_terminal_columns(),
           nspaces = 2, r, fill_len, i;
    assert(len < (max_len - nspaces));
    r = max_len - len;
    fill_len = (r - nspaces) / 2;
    if (conf.use_colors) printf("\033[1m");
    for (i = 0; i < fill_len; i++) printf("=");
    for (i = 0; i < (nspaces / 2); i++) printf(" ");
    printf("%s", str);
    for (i = 0; i < (nspaces / 2); i++) printf(" ");
    for (i = 0; i < fill_len; i++) printf("=");
    if (conf.use_colors) printf("\033[0m");
    printf("\n\n");
}

static void printFieldValuePair(char *field, const char *field_format, ...) {
    size_t len = strlen(field), fill_len, i;
    assert(len < LOG_MAX_FIELD_LEN);
    fill_len = LOG_MAX_FIELD_LEN - len;
    if (conf.use_colors) printf("\033[%dm", LOG_COLOR_CYAN);
    if (LOG_JUSTIFY_FIELD == LOG_JUSTIFY_RIGHT) {
        for (i = 0; i < fill_len; i++) printf(" ");
    }
    printf("%s: ", field);
    if (LOG_JUSTIFY_FIELD == LOG_JUSTIFY_LEFT) {
        for (i = 0; i < fill_len; i++) printf(" ");
    }
    if (conf.use_colors) printf("\033[0m");
    va_list ap;
    va_start(ap, field_format);
    vprintf(field_format, ap);
    va_end(ap);
    printf("\n");
}

static void logProgress(char *what, int current, int tot) {
    static int max_len = -1;
    if (max_len < 0) max_len = get_terminal_columns() - 1;
    int perc = ((float)(current / (float) tot)) * 100, i, r;
    int len = printf("\r%s: %d/%d (%d%%)", what, current, tot, perc);
    r = max_len - len;
    if (r > 0) {
        for (i = 0; i < r; i++) printf(" ");
    }
    fflush(stdout);
}

void printHelp(char **argv) {
    fprintf(stderr, "Usage: %s [OPTIONS] SER_MOVIE\n\n", argv[0]);
    fprintf(stderr, "OPTIONS:\n\n");
    fprintf(stderr, "   --extract FRAME_RANGE    Extract frames\n");
    fprintf(stderr, "   --cut FRAME_RANGE        Cut frames\n");
    fprintf(stderr, "   --split SPLIT            Split movie\n");
    fprintf(stderr, "   --json                   Log movie info to JSON\n");
    fprintf(stderr, "   --check                  Perform movie check before "
                                                 "any other action\n");
    fprintf(stderr, "   -o, --output FILE        Output movie path.\n");
    fprintf(stderr, "   --winjupos-format        Use WinJUPOS spec. for "
                                                 "output filename\n");
    fprintf(stderr, "   --overwrite              Force overwriting existing "
                                                 "files.\n");
    fprintf(stderr, "   --no-colors              Disable colored output\n");
    fprintf(stderr, "   -h, --help               Print this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "NOTES:\n\n");
    fprintf(stderr, "   * The value for FRAME_RANGE can be:\n");
    fprintf(stderr, "       <from>..<to>\n");
    fprintf(stderr, "       <from>,<count>\n");
    fprintf(stderr, "       <from>\n");
    fprintf(stderr, "     You can use negative value for <from> and <to>.\n"
        "     Example: -1 means the last frame\n\n"
    );
    fprintf(stderr, "   * Examples of value for SPLIT:\n");
    fprintf(stderr, "       --split  5      Split movie in 5 movies\n");
    fprintf(stderr, "       --split  10f    Split movie every 10 frames\n");
    fprintf(stderr, "       --split  10s    Split movie every 10 seconds\n\n");
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

int askForFileOverwrite(char *filepath) {
    char c, answer = '\0';
    int count = 0;
ask:
    logWarn("File '%s' already exists.\n", filepath);
    fprintf(stderr, "Overwrite it? (y/N) ");
    fflush(stderr);
    /*if (NULL == fgets(answer, sizeof(answer), stdin)) {
        fprintf(stderr, "\nError while reading from stdin\n");
        fflush(stderr);
        exit(1);
    }*/
    while ((c = getchar()) != '\n' && c != EOF) {
        if (answer == '\0') answer = c;
        count++;
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    if (answer == 'y' || answer == 'Y') return 1;
    else if (answer == 'n' || answer == 'N' || count == 0) return 0;
    else {
        answer = '\0';
        count = 0;
        goto ask;
    }
}

int parseFrameRangeArgument (char *arg) {
    uint32_t from = 0, to = 0, count = 0, last_n = 0;
    int is_comma = 0;
    size_t arglen = strlen(arg);
    if (arglen == 0) return 0;
    /* Search for separators: '-', ".." and ',' */
    char *sep = strstr(arg, "..");
    if (sep == NULL) sep = strchr(arg, ',');
    if (sep != NULL) {
        is_comma = (sep[0] == ',');
        int seplen = (is_comma ? 1 : 2);
        /* If arguments starts with separator (ie. ,1), it's invalid. */
        if (sep == arg) return 0;
        /* If arguments ends with separator (ie. 1,1), it's invalid. */
        if (sep == (arg + (arglen - seplen))) return 0;
        *sep = '\0';
        char *last_val = sep + seplen;
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

int determineSplitRanges(SERMovie *movie) {
    assert(movie->header != NULL);
    char *err = NULL;
    char errmsg[1024] = {0};
    uint32_t frames_to_add = movie->header->uiFrameCount,
             frames_added = 0, ranges_added = 0, last_frame_added = 0,
             last_movie_frame = movie->header->uiFrameCount - 1, i;
    time_t chuncks_duration[MAX_SPLIT_COUNT] = {0};
    if (conf.split_amount <= 0) {
        err = "invalid value";
        goto fail;
    }
    if (conf.split_mode<SPLIT_MODE_COUNT || conf.split_mode>SPLIT_MODE_SECS) {
        err = "invalid mode (see --help)";
        goto fail;
    }
    int min_src_frames = (
        MIN_SPLIT_FRAMES_PER_CHUNCK + (MIN_SPLIT_FRAMES_PER_CHUNCK / 2)
    );
    if (movie->header->uiFrameCount <= min_src_frames) {
        sprintf(errmsg, "at least %d frames needed in source movie",
            min_src_frames);
        err = errmsg;
        goto fail;
    }
    if (conf.split_mode == SPLIT_MODE_COUNT) {
        split_count = conf.split_amount;
        if (split_count > MAX_SPLIT_COUNT) goto max_split_count_exceeded;
        int frames_per_movie = movie->header->uiFrameCount / split_count,
            frames_added = 0, ranges_added = 0;
        if (frames_per_movie < MIN_SPLIT_FRAMES_PER_CHUNCK) {
            sprintf(errmsg,
                "too much splits, every chunck needs at least %d frames",
                MIN_SPLIT_FRAMES_PER_CHUNCK
            );
            err = errmsg;
            goto fail;
        }
        for (i = 0; i < split_count; i++) {
            uint32_t from = i * frames_per_movie;
            uint32_t to = from + frames_per_movie - 1;
            if (to >= movie->header->uiFrameCount) {
                to = movie->header->uiFrameCount - 1;
                break;
            }
            uint32_t count = 1 + (to - from);
            SERFrameRange *range = splitRanges + i;
            range->from = from;
            range->to = to;
            range->count = count;
            frames_added += count;
            ranges_added++;
        }
        if (frames_added < movie->header->uiFrameCount) {
            SERFrameRange *range = splitRanges + ranges_added;
            range->from = frames_added;
            range->to = movie->header->uiFrameCount - 1;
            updateRangeCount(range);
            frames_added += range->count;
            if (range->count < MIN_SPLIT_FRAMES_PER_CHUNCK) {
                if (ranges_added == 0) {
                    sprintf(errmsg, "at least %d frames needed",
                        MIN_SPLIT_FRAMES_PER_CHUNCK);
                    err = errmsg;
                    goto fail;
                }
                SERFrameRange *prev_range = range - 1;
                prev_range->to = range->to;
                updateRangeCount(prev_range);
            } else ranges_added++;
            split_count = ranges_added;
        }
    } else if (conf.split_mode == SPLIT_MODE_FRAMES) {
        if (conf.split_amount < MIN_SPLIT_FRAMES_PER_CHUNCK) {
            sprintf(errmsg, "every chunck needs at least %d frames",
                MIN_SPLIT_FRAMES_PER_CHUNCK);
            err = errmsg;
            goto fail;
        }
        while (frames_added < frames_to_add) {
            SERFrameRange *range = splitRanges + ranges_added;
            if (frames_added == 0) range->from = 0;
            else range->from = last_frame_added + 1;
            if (range->from > last_movie_frame) break;
            range->to = range->from + conf.split_amount - 1;
            if (range->to > last_movie_frame) range->to = last_movie_frame;
            updateRangeCount(range);
            frames_added += range->count;
            last_frame_added = range->to;
            if (++ranges_added > MAX_SPLIT_COUNT) {
                split_count = ranges_added;
                goto max_split_count_exceeded;
            }
        }
        split_count = ranges_added;
    } else if (conf.split_mode == SPLIT_MODE_SECS) {
        err = NULL;
        time_t previous_t = 0, elapsed_t = 0,
               max_t = (time_t) conf.split_amount,
               min_t = ((time_t) conf.split_amount) / 10;
        SERFrameRange *range = splitRanges;
        range->from = 0;
        range->to = 0;
        range->count = 0;
        for (i = 0; i < movie->header->uiFrameCount; i++) {
            uint64_t datetime = readFrameDate(movie, i);
            if (datetime == 0) goto invalid_datetime;
            time_t frame_t = videoTimeToUnixtime(datetime);
            if (frame_t <= 0 || frame_t < previous_t) goto invalid_datetime;
            if (i == 0) range->from = i;
            else if (i == range->from) {
                previous_t = frame_t;
                continue;
            } else if (previous_t > 0) {
                time_t last_frame_elapsed_t = (frame_t - previous_t);
                if (last_frame_elapsed_t > max_t) {
                    sprintf(errmsg, "too big time lapse between frame %d and "
                        "frame %d: %zu seconds",
                        i, i - 1, last_frame_elapsed_t
                    );
                    err = errmsg;
                    goto fail;
                }
                elapsed_t += last_frame_elapsed_t;
            }
            if (elapsed_t >= max_t) {
                uint32_t frame_idx = i;
                if (elapsed_t > max_t) frame_idx--;
                range->to = frame_idx;
                updateRangeCount(range);
                chuncks_duration[ranges_added] = elapsed_t;
                if (++ranges_added > MAX_SPLIT_COUNT) {
                    split_count = ranges_added;
                    goto max_split_count_exceeded;
                }
                if (range->count < MIN_SPLIT_FRAMES_PER_CHUNCK) {
                    sprintf(errmsg, "every chunck needs at least %d frames",
                        MIN_SPLIT_FRAMES_PER_CHUNCK);
                    err = errmsg;
                    goto fail;
                }
                range = splitRanges + ranges_added;
                range->from = frame_idx + 1;
                range->to = 0;
                range->count = 0;
                elapsed_t = 0;
            }
            previous_t = frame_t;
            continue;
invalid_datetime:
            sprintf(errmsg, "invalid datetime for frame %d", i);
            err = errmsg;
            break;
        }
        if (err != NULL) goto fail;
        if (range->from > 0 && range->from < last_movie_frame &&
            range->to == 0)
        {
            range->to = last_movie_frame;
            updateRangeCount(range);
            time_t duration = getFrameRangeDuration(movie, range);
            if (range->count < MIN_SPLIT_FRAMES_PER_CHUNCK || duration < min_t) 
            {
                if (ranges_added == 0) {
                    sprintf(errmsg, "at least %d frames needed",
                        MIN_SPLIT_FRAMES_PER_CHUNCK);
                    err = errmsg;
                    goto fail;
                }
                SERFrameRange *prev_range = range - 1;
                prev_range->to = range->to;
                updateRangeCount(prev_range);
            } else ranges_added++;
            chuncks_duration[ranges_added] = duration;
        }
        split_count = ranges_added;
        if (split_count > MAX_SPLIT_COUNT) goto max_split_count_exceeded;
    }
    int tot_frames_added = 0;
    time_t tot_time = 0;
    for (i = 0; i < split_count; i++) {
        SERFrameRange *range = splitRanges + i;
        time_t duration = chuncks_duration[i];
        if (duration == 0) {
            duration = getFrameRangeDuration(movie, range);
            if (duration < 0) {
                uint64_t start_date = readFrameDate(movie, range->from),
                         end_date = readFrameDate(movie, range->to);
                time_t start_t = videoTimeToUnixtime(start_date),
                       end_t = videoTimeToUnixtime(end_date);
                logErr(
                    LOG_TAG_FATAL
                    "End frame %d time %zu < start frame %d time %zu\n",
                    range->to, end_t, range->from, start_t
                );
                assert(end_t > start_t);
            }
        }
        tot_time += duration;
        printf("[%d] Split %d - %d (%d frames, %zu seconds)\n",
            i, range->from, range->to, range->count, duration);
        tot_frames_added += range->count;
    }
    if (tot_frames_added != movie->header->uiFrameCount) {
        logErr(LOG_TAG_FATAL "not all frames added %d/%d\n",
            tot_frames_added, movie->header->uiFrameCount);
        assert(tot_frames_added == movie->header->uiFrameCount);
    }
    printf("Average frames per chunck: %d\n", (tot_frames_added / split_count));
    printf("Average seconds per chunck: %ld\n\n",
        ((time_t)tot_time / split_count));
    return 1;
max_split_count_exceeded:
    sprintf(errmsg, "too much splits (%d), maximum splits allowed: %d",
        split_count, MAX_SPLIT_COUNT);
    err = errmsg;
fail:
    logErr(LOG_TAG_ERR "Unable to split movie");
    if (err != NULL) logErr(": %s", err);
    fprintf(stderr, "\n");
    return 0;
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
        int is_extract_opt = (strcmp("--extract", arg) == 0);
        int is_cut_opt = (strcmp("--cut", arg) == 0);
        int is_split_opt = (strcmp("--split", arg) == 0);
        if (is_extract_opt || is_cut_opt) {
            if (is_last_arg) {
                fprintf(stderr, "Missing value for `%s`\n", arg);
                exit(1);
            }
            char *frames = argv[++i];
            if (!parseFrameRangeArgument(frames)) goto invalid_range_arg;
            if (is_extract_opt) conf.action = ACTION_EXTRACT;
            else if (is_cut_opt) conf.action = ACTION_CUT;
        } else if (is_split_opt) {
            if (is_last_arg) {
                fprintf(stderr, "Missing value for `split`\n");
                exit(1);
            }
            char *split_val = argv[++i];
            size_t arglen = strlen(split_val);
            int split_mode = 0;
            char last_c = split_val[arglen - 1];
            if (last_c == 'f') split_mode = SPLIT_MODE_FRAMES;
            else if (last_c == 's') split_mode = SPLIT_MODE_SECS;
            else {
                if (last_c < '0' || last_c > '9') goto invalid_split_arg;
                split_mode = SPLIT_MODE_COUNT;
            }
            int split_amount = atoi(split_val);
            if (split_amount <= 0) goto invalid_split_arg;
            conf.split_amount = split_amount;
            conf.split_mode = split_mode;
            conf.action = ACTION_SPLIT;
        } else if (strcmp("--json", arg) == 0) {
            conf.log_to_json = 1;
        } else if (strcmp("--winjupos-format", arg) == 0) {
            conf.use_winjupos_filename = 1;
        } else if (strcmp("--check", arg) == 0) {
            conf.do_check = 1;
        } else if (strcmp("--overwrite", arg) == 0) {
            conf.overwrite = 1;
        } else if (strcmp("--no-colors", arg) == 0) {
            conf.use_colors = 0;
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
invalid_range_arg:
    fprintf(stderr, "Invalid frame range\n");
    exit(1);
    return -1;
invalid_split_arg:
    fprintf(stderr, "Invalid --split value\n");
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

void swapMovieHeader(SERHeader *header) {
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

int performMovieCheck(SERMovie *movie, int *issues) {
    assert(movie != NULL);
    int ok = 1, count = 0;
    printHeader("CHECK");
    printf("Checking for movie issues...\n");
    if (movie->header == NULL) {
        printf("FATAL: missing header\n");
        return 0;
    }
    if (movie->warnings != 0) {
        int wcount = countMovieWarnings(movie->warnings);
        logWarn("Found %d warnings:\n", wcount);
        printMovieWarnings(movie->warnings);
        count += wcount;
        ok = 0;
    }
    long trailer_offs = getTrailerOffset(movie->header);
    if (movie->filesize > trailer_offs) {
       int has_valid_dates = 1, i;
       uint64_t last_date = 0;
       for (i = 0; i < movie->header->uiFrameCount; i++) {
            uint64_t date = readFrameDate(movie, i);
            has_valid_dates = (last_date <= date);
            if (!has_valid_dates) break;
            last_date = date;
       }
       if (!has_valid_dates) {
            count += 1;
            logWarn("WARN: frame datetimes are not valid\n");
            ok = 0;
       }
    }
    if (issues != NULL) *issues = count;
    if (ok) logSuccess("Good, no issues found!\n\n");
    else printf("Found %d issue(s)\n\n", count);
    return ok;
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
            logErr(LOG_TAG_ERR "%s\n", err);
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
            logErr(LOG_TAG_ERR "Failed to read SER movie header\n");
            return 0;
        }
        if (totread == hdrsize) break;
        totread += nread;
        hdrptr += nread;
    }
    if (IS_BIG_ENDIAN) swapMovieHeader(movie->header);
    printf("Read %lu header bytes\n\n", totread);
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
    if (IS_BIG_ENDIAN && date > 0) swapint64(&date);
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
    if (count > 0) to = from + count - 1;
    else if (to < 0) to = tot + to;
    if (to >= tot) {
        if (err != NULL) *err = "last frame beyond movie frames";
        return 0;
    }
    if (to < from) {
        if (err != NULL) *err = "last frame < first frame";
        return 0;
    }
    range->from = from;
    range->to = to;
    updateRangeCount(range);
    return 1;
}

void closeMovie(SERMovie *movie) {
    if (movie == NULL) return;
    if (movie->header != NULL) free(movie->header);
    if (movie->file != NULL) fclose(movie->file);
    free(movie);
}

int countMovieWarnings(int warnings) {
    size_t len = sizeof(warnings), i;
    int count = 0;
    for (i = 0; i < len; i++) {
        if (warnings & (1 << i)) count++;
    }
    return count;
}

void printMovieWarnings(int warnings) {
    if (warnings & WARN_INCOMPLETE_FRAMES)
        logWarn("WARN: movie frames are incomplete\n");
    if (warnings & WARN_INCOMPLETE_TRAILER)
        logWarn("WARN: movie datetimes trailer is incomplete\n");
    if (warnings & WARN_BAD_FRAME_DATES)
        logWarn("WARN: frame datetimes order is wrong\n");
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
        logErr(LOG_TAG_ERR "%s\n", err);
        closeMovie(movie);
        return NULL;
    }
    if (!parseHeader(movie)) {
        logErr(LOG_TAG_ERR "Failed to parse movie header\n");
        closeMovie(movie);
        return NULL;
    }
    if (strcmp(SER_FILE_ID, movie->header->sFileID) != 0) {
        logErr(LOG_TAG_ERR "File is not a SER movie file\n");
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
    if (movie->warnings > 0 && !conf.do_check)
        printMovieWarnings(movie->warnings);
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

int writeTrailerToVideo(FILE *video, uint64_t *datetimes, size_t size) {
    size_t nwritten = 0, totwritten = 0, remain = size;
    char *ptr = (char *) datetimes;
    while (totwritten < size) {
        nwritten = fwrite(ptr, 1, remain, video);
        if (nwritten <= 0) break;
        totwritten += nwritten;
        ptr += nwritten;
        remain -= nwritten;
    }
    int ok = (totwritten == size);
    if (!ok) {
        fprintf(stderr, "Written %zu trailer byte(s) of %zu\n",
            totwritten, size);
    }
    return ok;
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
    FILE *ofile = NULL;
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
    if (fileExists(outputpath) && !conf.overwrite) {
        int overwrite = askForFileOverwrite(outputpath);
        if (!overwrite) goto fail;
    }
    ofile = fopen(outputpath, "w");
    if (ofile == NULL) {
        logErr(LOG_TAG_ERR "Failed to open %s for writing\n", outputpath);
        err = "could not open output video for writing";
        goto fail;
    }
    printHeader("EXTRACT FRAMES");
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
        int frame_id = i + 1;
        logProgress("Writing frames", frame_id, count);
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
    if (!writeTrailerToVideo(ofile, datetimes_buffer, trailer_size)) {
        err = "failed to write frame datetimes trailer";
        goto fail;
    }
    printf("New video written to:\n%s\n\n", outputpath);
    fflush(stdout);

    if (new_header != NULL) free(new_header);
    if (datetimes_buffer != NULL) free(datetimes_buffer);
    fclose(ofile);
    strcpy(output_movie_path, outputpath);
    return 1;
fail:
    if (ofile != NULL) fclose(ofile);
    if (new_header != NULL) free(new_header);
    if (datetimes_buffer != NULL) free(datetimes_buffer);
    logErr(LOG_TAG_ERR "Could not extract frames");
    if (err != NULL)
        logErr(": %s (frame count: %d)", err, header->uiFrameCount);
    fprintf(stderr, "\n");
    return 0;
}

int cutFramesFromVideo(SERMovie *movie, char *outputpath, SERFrameRange *range)
{
    char *err = NULL;
    SERHeader *new_header = NULL;
    uint64_t *datetimes_buffer = NULL;
    FILE *ofile = NULL;
    int i = 0;
    uint32_t from, to, count, tot_frames, first_frame_idx, last_frame_idx,
            src_last_frame;
    from = range->from;
    to = range->to;
    count = range->count;
    SERHeader *header = movie->header;
    if (header == NULL) {
        err = "missing source movie header";
        goto fail;
    }
    if (count >= movie->header->uiFrameCount) {
        err = "frames to cut must be less than source frame count";
        goto fail;
    }
    new_header = duplicateHeader(header);
    if (new_header == NULL) goto fail;
    tot_frames = movie->header->uiFrameCount - count;
    new_header->uiFrameCount = tot_frames;
    src_last_frame = (movie->header->uiFrameCount - 1);
    if (from == 0) first_frame_idx = to;
    else first_frame_idx = 0;
    if (to == src_last_frame) last_frame_idx = from;
    else last_frame_idx = src_last_frame;
    int64_t utc_diff = header->ulDateTime_UTC - header->ulDateTime;
    uint64_t first_frame_date = readFrameDate(movie, first_frame_idx);
    uint64_t first_frame_utc = first_frame_date;
    uint64_t last_frame_date = readFrameDate(movie, last_frame_idx);
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
    if (fileExists(outputpath) && !conf.overwrite) {
        int overwrite = askForFileOverwrite(outputpath);
        if (!overwrite) goto fail;
    }
    ofile = fopen(outputpath, "w");
    if (ofile == NULL) {
        logErr(LOG_TAG_ERR "Failed to open %s for writing\n", outputpath);
        err = "could not open output video for writing";
        goto fail;
    }
    printHeader("CUT FRAMES");
    printf("Cutting %d frame(s): %d - %d\n", count, from + 1, to + 1);
    printf("Total output frames: %d\n", tot_frames);
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
    size_t trailer_size = (tot_frames * sizeof(uint64_t));
    datetimes_buffer = malloc(trailer_size);
    if (datetimes_buffer == NULL) {
        err = "out-of-memory";
        goto fail;
    }
    int frame_idx, frame_id = 0;
    for (i = 0; i < from; i++) {
        frame_id = i + 1;
        frame_idx = i;
        logProgress("Writing frames", frame_id, tot_frames);
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
        datetimes_buffer[frame_idx] = datetime;
    }
    for (i = to + 1; i <= src_last_frame; i++) {
        frame_idx++;
        frame_id = frame_idx + 1;
        logProgress("Writing frames", frame_id, tot_frames);
        if (!appendFrameToVideo(ofile, movie, i, &err)) {
            printf("\n");
            fflush(stdout);
            goto fail;
        }
        uint64_t datetime = readFrameDate(movie, i);
        if (datetime == 0) {
            printf("\n");
            fflush(stdout);
            err = "invalid frame date";
            goto fail;
        }
        datetimes_buffer[frame_idx] = datetime;
    }
    printf("\n");
    fflush(stdout);
    printf("Writing frame datetimes trailer\n");
    if (!writeTrailerToVideo(ofile, datetimes_buffer, trailer_size)) {
        err = "failed to write frame datetimes trailer";
        goto fail;
    }
    printf("New video written to:\n%s\n", outputpath);
    fflush(stdout);

    if (new_header != NULL) free(new_header);
    if (datetimes_buffer != NULL) free(datetimes_buffer);
    fclose(ofile);
    strcpy(output_movie_path, outputpath);
    return 1;
fail:
    if (ofile != NULL) fclose(ofile);
    if (new_header != NULL) free(new_header);
    if (datetimes_buffer != NULL) free(datetimes_buffer);
    if (err != NULL) {
        logErr("Could not cut frames: %s (frame count: %d)\n",
            err, header->uiFrameCount);
    } else logErr("Could not cut frames\n");
    return 0;
}

int splitMovie(SERMovie *movie) {
    char *err = NULL;
    char errmsg[1024] = {0};
    if (split_count == 0) goto fail;
    assert(split_count <= MAX_SPLIT_COUNT);
    char **movie_files = malloc(split_count * sizeof(*movie_files));
    if (movie_files == NULL) {
        err = "Out-of-memory";
        goto fail;
    }
    memset(movie_files, 0, split_count * sizeof(*movie_files));
    int i, ok, extracted_movies = 0;
    for (i = 0; i < split_count; i++) {
        SERFrameRange *range = splitRanges + i;
        assert(range->from < range->to);
        assert(range->count > 0);
        ok = extractFramesFromVideo(movie, NULL, range);
        if (!ok) break;
        movie_files[extracted_movies++] = strdup(output_movie_path);
    }
print_and_free_movie_files:
    if (!ok) {
        if (extracted_movies <= 0) err = "no movies extracted";
        else {
            sprintf(errmsg, "only %d frame(s) extracted out of %d",
                extracted_movies, split_count
            );
            err = errmsg;
        }
    }
    if (movie_files != NULL) {
        if (extracted_movies > 0) printf("Files:\n\n");
        for (i = 0; i < extracted_movies; i++) {
            char *filepath = movie_files[i];
            if (filepath != NULL) printf("%s\n", filepath);
            free(filepath);
        }
        free(movie_files);
    }
    if (err != NULL) goto fail;
    return 1;
fail:
    logErr("Failed to split movie");
    if (err != NULL) logErr(": %s", err);
    fprintf(stderr, "\n");
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
    printFieldValuePair("File ID", "%s", fileID);
    printFieldValuePair("Little Endian", "%d", header->uiLittleEndian);
    printFieldValuePair("Color", "%s", getColorString(header->uiColorID));
    printFieldValuePair("Width", "%d", header->uiImageWidth);
    printFieldValuePair("Height", "%d", header->uiImageHeight);
    printFieldValuePair("Depth", "%d", header->uiPixelDepth);
    printFieldValuePair("Frames", "%d", header->uiFrameCount);
    printFieldValuePair("Observer", "%s", observer);
    printFieldValuePair("Camera", "%s", camera);
    printFieldValuePair("Telescope", "%s", scope);
    printFieldValuePair("Datetime", "%llu", header->ulDateTime);
    printFieldValuePair("Datetime (UTC)", "%llu", header->ulDateTime_UTC);
    printFieldValuePair("Datetime (UNIX)", "%ld", unix_t);
    printFieldValuePair("Timestamp", "%s", stripped_ctime(&unix_t));
    printFieldValuePair("Timestamp (UTC)", "%s", stripped_ctime(&unix_t_utc));
}

void printMovieInfo(SERMovie *movie) {
    printHeader("MOVIE INFO");
    if (movie->header != NULL) printMetadata(movie->header);
    printFieldValuePair("First Frame Date", "%llu", movie->firstFrameDate);
    printFieldValuePair("Last Frame Date", "%llu", movie->lastFrameDate);
    if (movie->firstFrameDate > 0) {
        time_t unix_t = videoTimeToUnixtime(movie->firstFrameDate);
        printFieldValuePair("First Frame Timestamp", "%s",
            stripped_ctime(&unix_t));
    }
    if (movie->firstFrameDate > 0) {
        time_t unix_t = videoTimeToUnixtime(movie->lastFrameDate);
        printFieldValuePair("Last Frame Timestamp", "%s",
            stripped_ctime(&unix_t));
    }
    char *fmt = NULL;
    if (movie->duration > 0) {
        char elapsed_time[255];
        elapsed_time[0] = '\0';
        fmt = "%d sec.%s";
        size_t elapsed_str_len =
            getElapsedTimeStr(elapsed_time, 255, movie->duration);
        if (elapsed_str_len > 0) fmt =  "%d sec. (%s)";
        printFieldValuePair("Duration", fmt, movie->duration, elapsed_time);
    }
    char fsize[255];
    fmt = "%ld%s";
    if (getFilesizeStr(fsize, 255, movie->filesize) > 0) fmt = "%ld (%s)";
    printFieldValuePair("Filesize", fmt, movie->filesize, fsize);
    if (movie->warnings != 0)
        printf("Found %d warning(s)\n", countMovieWarnings(movie->warnings));
    printf("\n");
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
        logErr(LOG_TAG_ERR "Could not open movie at: '%s'\n", filepath);
        goto err;
    }
    printMovieInfo(movie);
    int check_succeded = 1;
    if (conf.do_check) check_succeded = performMovieCheck(movie, NULL);
    SERHeader *header = movie->header;
    int action = conf.action;
    if (action < ACTION_SPLIT && action != ACTION_NONE && check_succeded) {
        /* Split or cut action */
        int from = conf.frames_from, to = conf.frames_to,
            count = conf.frames_count;
        SERFrameRange range;
        char *errmsg = NULL;
        if (!determineFrameRange(header, &range, from, to, count, &errmsg)) {
            logErr(LOG_TAG_ERR "Invalid frame range: ");
            if (errmsg == NULL) errmsg = "could not determine frame range";
            logErr("%s\n", errmsg);
            goto err;
        }
        char *output_path = conf.output_path;
        if (conf.use_winjupos_filename) output_path = NULL;
        int ok = 0;
        if (conf.action == ACTION_EXTRACT)
            ok = extractFramesFromVideo(movie, output_path, &range);
        else if (conf.action == ACTION_CUT)
            ok = cutFramesFromVideo(movie, output_path, &range);
        if (!ok) goto err;
        closeMovie(movie);
        return 0;
    } else if (conf.action == ACTION_SPLIT && check_succeded) {
        if (!determineSplitRanges(movie)) {
            logErr("Failed to split movie!\n");
            goto err;
        }
        if (!splitMovie(movie)) goto err;
    }
    if (conf.log_to_json) {
        char json_filename[BUFLEN];
        makeFilepath(json_filename, filepath, "/tmp/", NULL, ".json");
        int do_log = 1;
        if (fileExists(json_filename) && !conf.overwrite)
            do_log = askForFileOverwrite(json_filename);
        if (do_log) {
            FILE *json = fopen(json_filename, "w");
            if (json == NULL) {
                logErr(LOG_TAG_ERR "Could not open '%s' for writing!\n",
                    json_filename);
                return 1;
            }
            logToJSON(json, movie);
            printf("JSON saved to: '%s'\n", json_filename);
        }
    }
    closeMovie(movie);
    return 0;
err:
    closeMovie(movie);
    return 1;
}