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
#include "log.h"
#include "ser.h"
#include "fits.h"

#define ACTION_NONE         0
#define ACTION_EXTRACT      1
#define ACTION_CUT          2
#define ACTION_SPLIT        3
#define ACTION_SAVE_FRAME   4
#define ACTION_FIX          5

#define SPLIT_MODE_COUNT    1
#define SPLIT_MODE_FRAMES   2
#define SPLIT_MODE_SECS     3

#define MAX_SPLIT_COUNT             50
#define MIN_SPLIT_FRAMES_PER_CHUNCK 100

#define SIZE_KB 1024
#define SIZE_MB (SIZE_KB * 1024)
#define SIZE_GB (SIZE_MB * 1024)

#define BREAK_FRAMES        1
#define BREAK_DATES         2
#define BREAK_DATE_ORDER    3
#define BREAK_NO_DATES      4

#define IMAGE_FORMAT_RAW    1
#define IMAGE_FORMAT_FITS   2

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BUFLEN 255

#define getRangeCount(r) (1 + (r->to - r->from));
#define updateRangeCount(r) do {\
    assert(r->to >= r->from);\
    r->count = getRangeCount(r);\
} while (0);

typedef struct {
    uint32_t from;
    uint32_t to;
    uint32_t count;
} SERFrameRange;

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
    int break_movie; /* Used for tests */
    int save_frame_id;
    int image_format;
    int invert_endianness;
} MainConfig;

/* Globals */

MainConfig conf;
SERFrameRange splitRanges[MAX_SPLIT_COUNT] = {0};
uint32_t split_count = 0;
char output_movie_path[PATH_MAX + 1] = {0};
char *warn_messages[] = {
    WARN_FILESIZE_MISMATCH_MSG,
    WARN_INCOMPLETE_FRAMES_MSG,
    WARN_MISSING_TRAILER_MSG,
    WARN_INCOMPLETE_TRAILER_MSG,
    WARN_BAD_FRAME_DATES_MSG,
};
char *image_formats[] = {
    NULL,
    "raw",
    "fits"
};


/* Forward declarations */

static void printMovieWarnings(SERMovie *movie);

/* Utils */

static char *basename(char *path) {
    char *s = strrchr(path, '/');
    if (s == NULL) return path;
    else return s + 1;
}

static char *dirname(char *dst, char *path) {
    char *s = strrchr(path, '/');
    if (s == NULL) dst[0] = '\0';
    else {
        size_t len = s - path;
        strncpy(dst, path, len);
    }
    return dst;
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

static void printFieldValuePair(char *field, const char *field_format, ...) {
    size_t len = strlen(field), fill_len, i;
    assert(len < LOG_MAX_FIELD_LEN);
    fill_len = LOG_MAX_FIELD_LEN - len;
    if (SERLogUseColors) printf("\033[%dm", LOG_COLOR_CYAN);
    if (LOG_JUSTIFY_FIELD == LOG_JUSTIFY_RIGHT) {
        for (i = 0; i < fill_len; i++) printf(" ");
    }
    printf("%s: ", field);
    if (LOG_JUSTIFY_FIELD == LOG_JUSTIFY_LEFT) {
        for (i = 0; i < fill_len; i++) printf(" ");
    }
    if (SERLogUseColors) printf("\033[0m");
    va_list ap;
    va_start(ap, field_format);
    vprintf(field_format, ap);
    va_end(ap);
    printf("\n");
}


/* Program functions */

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

static time_t getFrameRangeDuration(SERMovie *movie, SERFrameRange *range) {
    if (range->to < range->from) return -1;
    if (range->to == range->from) return 0;
    uint64_t start_date = SERGetFrameDate(movie, range->from),
             end_date = SERGetFrameDate(movie, range->to);
    if (start_date == 0 || end_date == 0) return -1;
    if (end_date < start_date) return -1;
    if (start_date == end_date) return 0;
    time_t start_t = SERVideoTimeToUnixtime(start_date, NULL),
           end_t = SERVideoTimeToUnixtime(end_date, NULL);
    return end_t - start_t;
}

static int makeFilepath(char *dstfilepath, char *original_path, char *dir,
    char *suffix, char *ext)
{
    char *fname = basename(original_path);
    if (strlen(fname) == 0) {
        SERLogErr(LOG_TAG_FATAL
            "makeFilepath: `original_path` is a directory\n");
        return 0;
    }
    if (dir == NULL) dir = "/tmp/";
    size_t dirlen = strlen(dir);
    int has_sep = (dirlen > 0 && dir[dirlen - 1] == '/');
    int ext_has_dot = 0, fstem_len = strlen(fname);
    if (ext != NULL) {
        char *last_dot = strrchr(fname, '.');
        if (last_dot != NULL) fstem_len = last_dot - fname;
        ext_has_dot = (ext[0] == '.');
    }
    strcpy(dstfilepath, dir);
    if (!has_sep) strcat(dstfilepath, "/");
    strncat(dstfilepath, fname, fstem_len);
    if (suffix != NULL) strcat(dstfilepath, suffix);
    if (ext != NULL) {
        if (!ext_has_dot) strcat(dstfilepath, ".");
        strcat(dstfilepath, ext);
    }
    return 1;
}

static void freeWinJUPOSInfo(WinJUPOSInfo *info) {
    if (info == NULL) return;
    if (info->observer != NULL) free(info->observer);
    if (info->image_info != NULL) free(info->image_info);
    free(info);
}

static WinJUPOSInfo *getWinJUPOSInfo(char *filepath) {
    /* WinJUPOS filename spec: YYYY-mm-dd-HHMM[_T]-Observer[-ImageInfo] */
    WinJUPOSInfo *info = malloc(sizeof(*info));
    if (info == NULL) {
        SERLogErr(LOG_TAG_FATAL "Out-of-memory!\n");
        return NULL;
    }
    memset((void*) info, 0, sizeof(*info));
    char *filename = basename(filepath), *p, *last_p;
    char name_component[BUFLEN];
    name_component[0] = '\0';
    int namelen = strlen(filename);
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

static size_t generateWinJUPOSFilename(char *dst, size_t max_size, time_t time,
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

static size_t generateWinJUPOSMovieFilename(char *dst, size_t max_size,
    SERMovie *movie, char* ext)
{
    if (movie->warnings & WARN_BAD_FRAME_DATES) goto bad_dates;
    time_t start_t = SERVideoTimeToUnixtime(movie->firstFrameDate, NULL);
    time_t end_t = SERVideoTimeToUnixtime(movie->lastFrameDate, NULL);
    if (start_t <= 0 || end_t <= 0 || (end_t < start_t)) goto bad_dates;
    time_t mid_t = start_t + ((end_t - start_t) / 2);
    if (mid_t == 0) goto bad_dates;
    WinJUPOSInfo *wjinfo = getWinJUPOSInfo(movie->filepath);
    char info[PATH_MAX + 1];
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
        image_info = SERGetColorString(movie->header->uiColorID);
    strcat(info, "-");
    strcat(info, image_info);
    size_t sz = generateWinJUPOSFilename(dst, max_size, mid_t, info, ext);
    freeWinJUPOSInfo(wjinfo);
    return sz;
bad_dates:
    SERLogErr(LOG_TAG_ERR
        "Cannot generate WinJUPOS filename: bad datetimes\n");
    *dst = '\0';
    return 0;
}


static int makeMovieOutputPath(char *output_path, SERMovie *movie,
    SERFrameRange *range, char *dir)
{
    char suffix_buffer[BUFLEN];
    char wjfname[PATH_MAX + 1];
    char *filepath = movie->filepath, *suffix = NULL;
    int using_wjupos = 0, do_fix = (conf.action == ACTION_FIX);
    if (conf.use_winjupos_filename) {
        if (generateWinJUPOSMovieFilename(wjfname, PATH_MAX, movie, NULL) > 0) {
            using_wjupos = 1;
            filepath = wjfname;
        } else {
            SERLogErr("Could not generate WinJUPOS filename\n");
            return 0;
        }
    }
    if (!using_wjupos && range != NULL && conf.break_movie == 0 && !do_fix) {
        char *fmt = (conf.action == ACTION_CUT ? "-%d-%d-cut" : "-%d-%d");
        sprintf(suffix_buffer, fmt, range->from + 1, range->to + 1);
        suffix = suffix_buffer;
    } else if (do_fix) {
        suffix = "-fixed";
    } else if (conf.break_movie > 0) {
        switch (conf.break_movie) {
        case BREAK_FRAMES:
            suffix = "-broken-frames";
            break;
        case BREAK_DATES:
            suffix = "-broken-dates";
            break;
        case BREAK_DATE_ORDER:
            suffix = "-broken-date-order";
            break;
        case BREAK_NO_DATES:
            suffix = "-broken-no-dates";
            break;
        default:
            suffix = "-broken";
        }
    }
    if (dir == NULL) dir = conf.output_dir;
    if (dir == NULL) dir = "/tmp/";
    if (!makeFilepath(output_path, filepath, dir, suffix, ".ser")) {
        SERLogErr("Failed to create temporary filepath\n");
        return 0;
    }
    return 1;
}

static int determineSplitRanges(SERMovie *movie) {
    assert(movie->header != NULL);
    char *err = NULL;
    char errmsg[1024] = {0};
    uint32_t frames_to_add = SERGetFrameCount(movie),
             frames_added = 0, ranges_added = 0, last_frame_added = 0,
             last_movie_frame = SERGetLastFrameIndex(movie), i;
    time_t chuncks_duration[MAX_SPLIT_COUNT] = {0};
    if (conf.split_amount <= 0) {
        err = "invalid value";
        goto fail;
    }
    if (conf.split_mode<SPLIT_MODE_COUNT || conf.split_mode>SPLIT_MODE_SECS) {
        err = "invalid mode (see --help)";
        goto fail;
    }
    uint32_t min_src_frames = (
        MIN_SPLIT_FRAMES_PER_CHUNCK + (MIN_SPLIT_FRAMES_PER_CHUNCK / 2)
    );
    if (SERGetFrameCount(movie) <= min_src_frames) {
        sprintf(errmsg, "at least %d frames needed in source movie",
            min_src_frames);
        err = errmsg;
        goto fail;
    }
    if (conf.split_mode == SPLIT_MODE_COUNT) {
        split_count = conf.split_amount;
        if (split_count > MAX_SPLIT_COUNT) goto max_split_count_exceeded;
        uint32_t frames_per_movie = SERGetFrameCount(movie) / split_count,
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
            if (to >= SERGetFrameCount(movie)) {
                to = SERGetLastFrameIndex(movie);
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
        if (frames_added < SERGetFrameCount(movie)) {
            SERFrameRange *range = splitRanges + ranges_added;
            range->from = frames_added;
            range->to = SERGetLastFrameIndex(movie);
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
        for (i = 0; i < SERGetFrameCount(movie); i++) {
            uint64_t datetime = SERGetFrameDate(movie, i);
            if (datetime == 0) goto invalid_datetime;
            time_t frame_t = SERVideoTimeToUnixtime(datetime, NULL);
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
    uint32_t tot_frames_added = 0;
    time_t tot_time = 0;
    for (i = 0; i < split_count; i++) {
        SERFrameRange *range = splitRanges + i;
        time_t duration = chuncks_duration[i];
        if (duration == 0) {
            duration = getFrameRangeDuration(movie, range);
            if (duration < 0) {
                uint64_t start_date = SERGetFrameDate(movie, range->from),
                         end_date = SERGetFrameDate(movie, range->to);
                time_t start_t = SERVideoTimeToUnixtime(start_date, NULL),
                       end_t = SERVideoTimeToUnixtime(end_date, NULL);
                SERLogErr(
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
    if (tot_frames_added != SERGetFrameCount(movie)) {
        SERLogErr(LOG_TAG_FATAL "not all frames added %d/%d\n",
            tot_frames_added, SERGetFrameCount(movie));
        assert(tot_frames_added == SERGetFrameCount(movie));
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
    SERLogErr(LOG_TAG_ERR "Unable to split movie");
    if (err != NULL) SERLogErr(": %s", err);
    fprintf(stderr, "\n");
    return 0;
}

static void initConfig() {
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
    conf.break_movie = 0;
    conf.image_format = 0;
    conf.save_frame_id = 0;
    conf.invert_endianness = 0;
    SERLogUseColors = 1;
    SERLogLevel = LOG_LEVEL_INFO;
}


static void printHelp(char **argv) {
    fprintf(stderr, "serutils v%s\n\n", SERUTILS_VERSION);
    fprintf(stderr, "Usage: %s [OPTIONS] SER_MOVIE_PATH\n\n", argv[0]);
    fprintf(stderr, "OPTIONS:\n\n");
    fprintf(stderr, "   --extract FRAME_RANGE    Extract frames\n");
    fprintf(stderr, "   --cut FRAME_RANGE        Cut frames\n");
    fprintf(stderr, "   --split SPLIT            Split movie\n");
    fprintf(stderr, "   --save-frame FRAME_ID    Save frame\n");
    fprintf(stderr, "   --check                  Perform movie check before "
                                                 "any other action\n");
    fprintf(stderr, "   --fix                    Try to fix movie if needed."
                                                 "\n");
    fprintf(stderr, "   --image-format [FORMAT]  Image format for --save-frame"
                                                 " action.\n"
                    "                            Leave it empty to get a list "
                    "of supported formats.\n");
    fprintf(stderr, "   --invert-endianness      Invert movie endianness "
                                                 "specified in movie header\n");
    fprintf(stderr, "   -o, --output FILE        Output movie path.\n");
    fprintf(stderr, "   --json                   Log movie info to JSON\n");
    fprintf(stderr, "   --winjupos-format        Use WinJUPOS spec. for "
                                                 "output filename\n");
    fprintf(stderr, "   --overwrite              Force overwriting existing "
                                                 "files.\n");
    fprintf(stderr, "   --no-colors              Disable colored output\n");
    fprintf(stderr, "   --version                Print version\n");
    fprintf(stderr, "   -h, --help               Print this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "NOTES:\n\n");
    fprintf(stderr, "   * The value for FRAME_RANGE can be:\n");
    fprintf(stderr, "       <from>..<to>\n");
    fprintf(stderr, "       <from>,<count>\n");
    fprintf(stderr, "       <count>\n");
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
}

static int askForFileOverwrite(char *filepath) {
    char c, answer = '\0';
    int count = 0;
ask:
    SERLogWarn("File '%s' already exists.\n", filepath);
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

static int parseFrameRangeArgument (char *arg) {
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
        count = atoi(arg);
        if (count == 0) return 0;
        from = 1;
    }
    if (from > 0) from--;
    if (to > 0) to--;
    conf.frames_from = from;
    conf.frames_to = to;
    conf.frames_count = count;
    return 1;
}

static int parseOptions(int argc, char **argv) {
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
        } else if (strcmp("--save-frame", arg) == 0) {
            if (is_last_arg) {
                fprintf(stderr, "Missing frame id for `--save-frame`\n");
                exit(1);
            }
            conf.save_frame_id = atoi(argv[++i]);
            conf.action = ACTION_SAVE_FRAME;
            if (conf.image_format == 0)
                conf.image_format = IMAGE_FORMAT_FITS;
        } else if (strcmp("--image-format", arg) == 0) {
            if (is_last_arg) goto print_image_formats;
            char *format = argv[++i];
            conf.image_format = 0;
            int j, numformats = (int)(sizeof(image_formats) / sizeof(char *));
            for (j = 0; j < numformats; j++) {
                char *fmt = image_formats[j];
                if (fmt == NULL) continue;
                if (strcasecmp(format, fmt) == 0) {
                    conf.image_format = j;
                    break;
                }
            }
            if (conf.image_format == 0) {
                fprintf(stderr, "Invalid image format\n");
                goto print_image_formats;
            }
        } else if (strcmp("--json", arg) == 0) {
            conf.log_to_json = 1;
        } else if (strcmp("--winjupos-format", arg) == 0) {
            conf.use_winjupos_filename = 1;
        } else if (strcmp("--check", arg) == 0) {
            conf.do_check = 1;
        } else if (strcmp("--fix", arg) == 0) {
            conf.do_check = 1;
            conf.action = ACTION_FIX;
        } else if (strcmp("--overwrite", arg) == 0) {
            conf.overwrite = 1;
        } else if (strcmp("--invert-endianness", arg) == 0) {
            conf.invert_endianness = 1;
        } else if (strcmp("--no-colors", arg) == 0) {
            SERLogUseColors = 0;
        } else if (strcmp("-o", arg) == 0 || strcmp("--output", arg) == 0) {
            if (is_last_arg) {
                fprintf(stderr, "Missing value for output\n");
                exit(1);
            }
            conf.output_path = argv[++i];
        /* Used for tests */
        } else if (strcmp("--break-frames", arg) == 0) {
            conf.break_movie = BREAK_FRAMES;
        } else if (strcmp("--break-dates", arg) == 0) {
            conf.break_movie = BREAK_DATES;
        } else if (strcmp("--break-date-order", arg) == 0) {
            conf.break_movie = BREAK_DATE_ORDER;
        } else if (strcmp("--break-no-dates", arg) == 0) {
            conf.break_movie = BREAK_NO_DATES;
        } else if (strcmp("--version", arg) == 0) {
            printf("%s\n", SERUTILS_VERSION);
            exit(0);
        } else if (strcmp("-h", arg) == 0 || strcmp("--help", arg) == 0) {
            printHelp(argv);
            exit(1);
        } else if (arg[0] == '-') {
            fprintf(stderr, "Invalid argument `%s`\n", arg);
            exit(1);
        }
        else break;
    }
    if (conf.break_movie > 0) {
        conf.action = ACTION_EXTRACT;
        conf.frames_from = 0;
        if (conf.break_movie == BREAK_FRAMES) conf.frames_to = -2;
        else conf.frames_to = -1;
        conf.use_winjupos_filename = 0;
    } else if (conf.action == ACTION_SAVE_FRAME)
        conf.use_winjupos_filename = 0;
    return i;
invalid_range_arg:
    fprintf(stderr, "Invalid frame range\n");
    exit(1);
    return -1;
invalid_split_arg:
    fprintf(stderr, "Invalid --split value\n");
    exit(1);
    return -1;
print_image_formats:
    fprintf(stderr, "Supported image formats:\n");
    for (i = 0; i < (int)(sizeof(image_formats) / sizeof(char *)); i++) {
        char *format = image_formats[i];
        if (format == NULL) continue;
        fprintf(stderr, "    %s\n", format);
    }
    exit(1);
    return -1;
}

static void printPixelValue(SERMovie *movie, uint32_t frame_idx, uint32_t x,
    uint32_t y)
{
    if (movie->warnings & WARN_INCOMPLETE_FRAMES) {
        SERLogErr(LOG_TAG_ERR "movie frames are incomplete\n");
        return;
    }
    assert(movie->header != NULL);
    if (frame_idx > SERGetFrameCount(movie)) {
        SERLogErr(LOG_TAG_ERR "frame %d beyond movie frames (%d)\n",
            frame_idx, SERGetFrameCount(movie));
        return;
    }
    SERFrame *frame = SERGetFrame(movie, frame_idx);
    if (frame == NULL) {
        SERLogErr(LOG_TAG_ERR "unable to get frame %d\n", frame_idx);
        return;
    }
    SERPixelValue px;
    if (SERGetFramePixel(movie, frame, x, y, IS_BIG_ENDIAN, &px)) {
        if (frame->colorID < COLOR_RGB) {
            if (frame->pixelDepth > 8) printf("%d\n", px.int16);
            else printf("%d\n", px.int8);
        } else {
            if (frame->pixelDepth > 8)
                printf("%d,%d,%d\n", px.rgb16.r, px.rgb16.g, px.rgb16.b);
            else
                printf("%d,%d,%d\n", px.rgb8.r, px.rgb8.g, px.rgb8.b);
        }
    }
    SERReleaseFrame(frame);
}

static int performMovieCheck(SERMovie *movie, int *issues) {
    assert(movie != NULL);
    int ok = 1, count = 0;
    SERPrintHeader("CHECK");
    printf("Checking for movie issues...\n");
    if (movie->header == NULL) {
        SERLogErr(LOG_TAG_FATAL "missing header\n");
        return 0;
    }
    size_t trailer_offs = SERGetTrailerOffset(movie->header);
    uint32_t frame_c = SERGetFrameCount(movie);
    size_t expected_filesize = sizeof(SERHeader) +
        (frame_c * SERGetFrameSize(movie->header));
    if (movie->filesize > trailer_offs) {
        int has_valid_dates = 1;
        uint32_t i;
        uint64_t last_date = 0;
        expected_filesize += (frame_c * sizeof(uint64_t));
        for (i = 0; i < SERGetFrameCount(movie); i++) {
            uint64_t date = SERGetFrameDate(movie, i);
            has_valid_dates = (last_date <= date);
            if (!has_valid_dates) break;
            last_date = date;
        }
        if (movie->filesize < expected_filesize)
            movie->warnings |= WARN_INCOMPLETE_TRAILER;
        else if (!has_valid_dates) movie->warnings |= WARN_BAD_FRAME_DATES;
    }
    if (movie->filesize > expected_filesize) {
        SERLogWarn(LOG_TAG_WARN "%s\n", WARN_FILESIZE_MISMATCH_MSG);
        movie->warnings |= WARN_FILESIZE_MISMATCH;
    }
    if (movie->warnings != 0) {
        int wcount = SERCountMovieWarnings(movie->warnings);
        SERLogWarn("Found %d warning(s):\n", wcount);
        printMovieWarnings(movie);
        count += wcount;
    }
    ok = (count == 0);
    if (issues != NULL) *issues = count;
    if (ok) SERLogSuccess("Good, no issues found!\n\n");
    else SERLogWarn("Found %d issue(s)\n\n", count);
    return ok;
}

static int determineFrameRange(SERHeader * header, SERFrameRange *range,
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

static void printMovieWarnings(SERMovie *movie) {
    int warnings = movie->warnings;
    size_t wlen = sizeof(warnings),
           msgcount = sizeof(warn_messages) / sizeof(char *), i;
    for (i = 0; i < wlen; i++) {
        if (i >= msgcount) break;
        if (warnings & (1 << i)) {
            char *wmsg = warn_messages[i];
            if (wmsg == NULL) break;
            SERLogWarn(LOG_TAG_WARN "%s\n", wmsg);
            if (wmsg == WARN_INCOMPLETE_FRAMES_MSG) {
                int frame_count = SERGetRealFrameCount(movie);
                SERLogWarn(" !! Movie has %d frame(s), but there "
                    "should be %d frame(s)\n",
                    frame_count, SERGetFrameCount(movie));
            }
        }
    }
}

static int writeHeaderToVideo(FILE *video, SERHeader *header) {
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

static int writeTrailerToVideo(FILE *video, uint64_t *datetimes, size_t size) {
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

static int appendFrameToVideo(FILE *video, SERMovie *srcmovie,
    int frame_idx, char **err)
{
    SERHeader *srcheader = srcmovie->header;
    FILE *srcvideo = srcmovie->file;
    size_t frame_sz = SERGetFrameSize(srcheader), nread = 0, nwritten = 0,
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
    long offset = SERGetFrameOffset(srcheader, frame_idx);
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

static int extractFramesFromVideo(SERMovie *movie, char *outputpath,
    SERFrameRange *range)
{
    char *err = NULL;
    SERHeader *new_header = NULL;
    uint64_t *datetimes_buffer = NULL;
    FILE *ofile = NULL;
    uint32_t from, to, count, i = 0;
    int do_fix = (conf.action == ACTION_FIX),
        has_trailer = SERMovieHasTrailer(movie);
    from = range->from;
    to = range->to;
    count = range->count;
    SERHeader *header = movie->header;
    if (header == NULL) {
        err = "missing source movie header";
        goto fail;
    }
    new_header = SERDuplicateHeader(header);
    if (new_header == NULL) goto fail;
    if (conf.break_movie != BREAK_FRAMES)
        new_header->uiFrameCount = count;
    int64_t utc_diff = header->ulDateTime_UTC - header->ulDateTime;
    uint64_t first_frame_date, last_frame_date, first_frame_utc;
    if (has_trailer) {
        first_frame_date = SERGetFrameDate(movie, from);
        first_frame_utc = first_frame_date;
        last_frame_date = SERGetFrameDate(movie, to);
        if (first_frame_date == 0 && !do_fix) {
            err = "unable to read first frame date";
            goto fail;
        }
        if (utc_diff > 0 && (uint64_t) utc_diff < first_frame_utc)
            first_frame_utc -= utc_diff;
        new_header->ulDateTime = first_frame_date;
        new_header->ulDateTime_UTC = first_frame_utc;
    }
    if (outputpath == NULL) {
        char opath[PATH_MAX + 1];
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
        SERLogErr(LOG_TAG_ERR "Failed to open %s for writing\n", outputpath);
        err = "could not open output video for writing";
        goto fail;
    }
    SERPrintHeader("EXTRACT FRAMES");
    printf("Extracting %d frame(s): %d - %d\n", count, from + 1, to + 1);
    printf("Writing movie header\n");
    if (!writeHeaderToVideo(ofile, new_header)) {
        err = "failed to write header";
        goto fail;
    }
    long offset = SERGetFrameOffset(header, from);
    if (fseek(movie->file, offset, SEEK_SET) < 0) {
        err = "frame offset beyond movie length";
        goto fail;
    }
    size_t trailer_size = (count * sizeof(uint64_t));
    uint32_t broken_dates_count = 0;
    if (conf.break_movie == BREAK_DATES) {
        broken_dates_count = (count > 1 ? 2 : count);
        trailer_size = (broken_dates_count * sizeof(uint64_t));
    }
    if (has_trailer) {
        datetimes_buffer = malloc(trailer_size);
        if (datetimes_buffer == NULL) {
            err = "out-of-memory";
            goto fail;
        }
    }
    for (i = 0; i < count; i++) {
        int frame_id = i + 1;
        SERLogProgress("Writing frames", frame_id, count);
        int frame_idx = from + i;
        if (!appendFrameToVideo(ofile, movie, frame_idx, &err)) {
            printf("\n");
            fflush(stdout);
            goto fail;
        }
        if (datetimes_buffer == NULL) continue;
        uint64_t datetime = SERGetFrameDate(movie, frame_idx);
        if (datetime == 0) {
            printf("\n");
            fflush(stdout);
            err = "invalid frame date";
            goto fail;
        }
        if (broken_dates_count > 0 && i >= broken_dates_count) continue;
        datetimes_buffer[i] = datetime;
    }
    printf("\n");
    fflush(stdout);
    if (has_trailer && datetimes_buffer != NULL) {
        if (conf.break_movie == BREAK_DATE_ORDER && count > 1) {
            uint64_t first_date = datetimes_buffer[0],
                     last_date = datetimes_buffer[count - 1];
            datetimes_buffer[0] = last_date;
            datetimes_buffer[1] = first_date;
        } else if (conf.break_movie == BREAK_NO_DATES) goto final;
        printf("Writing frame datetimes trailer\n");
        if (!writeTrailerToVideo(ofile, datetimes_buffer, trailer_size)) {
            err = "failed to write frame datetimes trailer";
            goto fail;
        }
    }
final:
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
    SERLogErr(LOG_TAG_ERR "Could not extract frames");
    if (err != NULL)
        SERLogErr(": %s (frame count: %d)", err, header->uiFrameCount);
    fprintf(stderr, "\n");
    return 0;
}

static int cutFramesFromVideo(SERMovie *movie, char *outputpath,
    SERFrameRange *range)
{
    char *err = NULL;
    SERHeader *new_header = NULL;
    uint64_t *datetimes_buffer = NULL;
    FILE *ofile = NULL;
    uint32_t from, to, count, tot_frames, first_frame_idx, last_frame_idx,
            src_last_frame, i = 0;
    from = range->from;
    to = range->to;
    count = range->count;
    SERHeader *header = movie->header;
    if (header == NULL) {
        err = "missing source movie header";
        goto fail;
    }
    if (count >= SERGetFrameCount(movie)) {
        err = "frames to cut must be less than source frame count";
        goto fail;
    }
    new_header = SERDuplicateHeader(header);
    if (new_header == NULL) goto fail;
    tot_frames = SERGetFrameCount(movie) - count;
    new_header->uiFrameCount = tot_frames;
    src_last_frame = SERGetLastFrameIndex(movie);
    if (from == 0) first_frame_idx = to;
    else first_frame_idx = 0;
    if (to == src_last_frame) last_frame_idx = from;
    else last_frame_idx = src_last_frame;
    int64_t utc_diff = header->ulDateTime_UTC - header->ulDateTime;
    uint64_t first_frame_date = SERGetFrameDate(movie, first_frame_idx);
    uint64_t first_frame_utc = first_frame_date;
    uint64_t last_frame_date = SERGetFrameDate(movie, last_frame_idx);
    if (first_frame_date == 0) {
        err = "unable to read first frame date";
        goto fail;
    }
    if (utc_diff > 0 && (uint64_t) utc_diff < first_frame_utc)
        first_frame_utc -= utc_diff;
    new_header->ulDateTime = first_frame_date;
    new_header->ulDateTime_UTC = first_frame_utc;
    if (outputpath == NULL) {
        char opath[PATH_MAX + 1];
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
        SERLogErr(LOG_TAG_ERR "Failed to open %s for writing\n", outputpath);
        err = "could not open output video for writing";
        goto fail;
    }
    SERPrintHeader("CUT FRAMES");
    printf("Cutting %d frame(s): %d - %d\n", count, from + 1, to + 1);
    printf("Total output frames: %d\n", tot_frames);
    printf("Writing movie header\n");
    if (!writeHeaderToVideo(ofile, new_header)) {
        err = "failed to write header";
        goto fail;
    }
    long offset = SERGetFrameOffset(header, from);
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
        SERLogProgress("Writing frames", frame_id, tot_frames);
        if (!appendFrameToVideo(ofile, movie, frame_idx, &err)) {
            printf("\n");
            fflush(stdout);
            goto fail;
        }
        uint64_t datetime = SERGetFrameDate(movie, frame_idx);
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
        SERLogProgress("Writing frames", frame_id, tot_frames);
        if (!appendFrameToVideo(ofile, movie, i, &err)) {
            printf("\n");
            fflush(stdout);
            goto fail;
        }
        uint64_t datetime = SERGetFrameDate(movie, i);
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
        SERLogErr("Could not cut frames: %s (frame count: %d)\n",
            err, header->uiFrameCount);
    } else SERLogErr("Could not cut frames\n");
    return 0;
}

static int splitMovie(SERMovie *movie) {
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
    for (i = 0; i < (int)split_count; i++) {
        SERFrameRange *range = splitRanges + i;
        assert(range->from < range->to);
        assert(range->count > 0);
        ok = extractFramesFromVideo(movie, NULL, range);
        if (!ok) break;
        movie_files[extracted_movies++] = strdup(output_movie_path);
    }
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
    SERLogErr("Failed to split movie");
    if (err != NULL) SERLogErr(": %s", err);
    fprintf(stderr, "\n");
    return 0;
}

static void printMetadata(SERHeader *header) {
    char fileID[15];
    char observer[40];
    char scope[40];
    char camera[40];
    strncpy(fileID, (const char*) header->sFileID, 14);
    strncpy(observer, (const char*) header->sObserver, 40);
    strncpy(camera, (const char*) header->sInstrument, 40);
    strncpy(scope, (const char*) header->sTelescope, 40);
    time_t unix_t = SERVideoTimeToUnixtime(header->ulDateTime, NULL);
    time_t unix_t_utc = SERVideoTimeToUnixtime(header->ulDateTime_UTC, NULL);
    fileID[14] = '\0';
    observer[39] = '\0';
    scope[39] = '\0';
    camera[39] = '\0';
    printFieldValuePair("File ID", "%s", fileID);
    printFieldValuePair("Little Endian", "%d", header->uiLittleEndian);
    printFieldValuePair("Color", "%s", SERGetColorString(header->uiColorID));
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

static void printMovieInfo(SERMovie *movie) {
    SERPrintHeader("MOVIE INFO");
    char fsize[BUFLEN];
    if (movie->header != NULL) printMetadata(movie->header);
    if (!SERMovieHasTrailer(movie)) {
        printFieldValuePair("Frame dates", "%s", "missing");
        goto print_filesize;
    }
    printFieldValuePair("First Frame Date", "%llu", movie->firstFrameDate);
    printFieldValuePair("Last Frame Date", "%llu", movie->lastFrameDate);
    if (movie->firstFrameDate > 0) {
        time_t unix_t = SERVideoTimeToUnixtime(movie->firstFrameDate, NULL);
        printFieldValuePair("First Frame Timestamp", "%s",
            stripped_ctime(&unix_t));
    }
    if (movie->firstFrameDate > 0) {
        time_t unix_t = SERVideoTimeToUnixtime(movie->lastFrameDate, NULL);
        printFieldValuePair("Last Frame Timestamp", "%s",
            stripped_ctime(&unix_t));
    }
    char *fmt = NULL;
    if (movie->duration > 0) {
        char elapsed_time[BUFLEN];
        elapsed_time[0] = '\0';
        fmt = "%d sec.%s";
        size_t elapsed_str_len =
            getElapsedTimeStr(elapsed_time, BUFLEN, movie->duration);
        if (elapsed_str_len > 0) fmt =  "%d sec. (%s)";
        printFieldValuePair("Duration", fmt, movie->duration, elapsed_time);
        float fps = (float) SERGetFrameCount(movie) / (float) movie->duration;
        printFieldValuePair("FPS", "%.2f", fps);
    }
print_filesize:
    fmt = "%ld%s";
    if (getFilesizeStr(fsize, BUFLEN, movie->filesize) > 0) fmt = "%ld (%s)";
    printFieldValuePair("Filesize", fmt, movie->filesize, fsize);
    if (movie->warnings != 0) {
        SERLogWarn("Found %d warning(s)\n",
            SERCountMovieWarnings(movie->warnings));
    }
    printf("\n");
}

static int logToJSON(FILE *json_file, SERMovie *movie)
{
    char fileID[15];
    char observer[40];
    char scope[40];
    char camera[40];
    char *abspath = NULL;
    SERHeader *header = movie->header;
    strncpy(fileID, (const char*) header->sFileID, 14);
    strncpy(observer, (const char*) header->sObserver, 40);
    strncpy(camera, (const char*) header->sInstrument, 40);
    strncpy(scope, (const char*) header->sTelescope, 40);
    fileID[14] = '\0';
    observer[39] = '\0';
    scope[39] = '\0';
    camera[39] = '\0';
    if (movie->filepath != NULL) abspath = realpath(movie->filepath, NULL);
    fprintf(json_file, "{\n");
    fprintf(json_file, "    \"path\": \"%s\",\n",
        (abspath != NULL ? abspath : movie->filepath));
    fprintf(json_file, "    \"fileID\": \"%s\",\n", fileID);
    fprintf(json_file, "    \"littleEndian\": %u,\n",
        header->uiLittleEndian);
    fprintf(json_file, "    \"color\": \"%s\",\n",
        SERGetColorString(header->uiColorID));
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
    fprintf(json_file, "    \"lastFrameDatetime\": %llu,\n",
        movie->lastFrameDate);

    fprintf(json_file, "    \"unixtime\": %zu,\n",
        SERVideoTimeToUnixtime(header->ulDateTime, NULL));
    fprintf(json_file, "    \"unixtimeUTC\": %zu,\n",
        SERVideoTimeToUnixtime(header->ulDateTime_UTC, NULL));
    fprintf(json_file, "    \"firstFrameUnixtime\": %zu,\n",
        SERVideoTimeToUnixtime(movie->firstFrameDate, NULL));
    fprintf(json_file, "    \"lastFrameUnixtime\": %zu,\n",
        SERVideoTimeToUnixtime(movie->lastFrameDate, NULL));
    fprintf(json_file, "    \"duration\": %d,\n", movie->duration);

    fprintf(json_file, "    \"warnings\": [");
    size_t wlen = sizeof(movie->warnings),
           msgcount = sizeof(warn_messages) / sizeof(char *), count = 0, i;
    for (i = 0; i < wlen; i++) {
        if (i >= msgcount) break;
        if (movie->warnings & (1 << i)) {
            char *wmsg = warn_messages[i];
            char *comma = "", *endl = "";
            if (wmsg == NULL) break;
            if (count++ > 0) comma = ",\n";
            else comma = "\n";
            //else endl = "\n";
            fprintf(json_file, "%s        \"%s\"%s", comma, wmsg, endl);
        }
    }
    fprintf(json_file, "\n    ]\n");

    fprintf(json_file, "}\n");
    if (abspath != NULL) free(abspath);
    return 1;
}

static int saveFITSImage(SERMovie *movie, FILE *imagefile, uint32_t frame_idx,
    void *pixels, size_t size)
{
    FITSHeaderUnit *hdr = NULL;
    void *data_unit =  NULL;
    size_t data_size = 0;
    hdr = FITSCreateHeaderUnit();
    int keyword_added =
        FITSHeaderAdd(hdr, "SIMPLE", "file does conform to FITS standard", "T");
    if (!keyword_added) goto keyword_fail;
    uint32_t color_id = movie->header->uiColorID;
    int bitpix = (movie->header->uiPixelDepth <= 8 ? 8 : 16),
        is_mono = (color_id < COLOR_RGB),
        naxis = (is_mono ? 2 : 3);
    keyword_added = FITSHeaderAdd(hdr, "BITPIX",
        "number of bits per data pixel", "%d", bitpix);
    if (!keyword_added) goto keyword_fail;
    keyword_added = FITSHeaderAdd(hdr, "NAXIS",
        "number of data axes", "%d", naxis);
    if (!keyword_added) goto keyword_fail;
    keyword_added = FITSHeaderAdd(hdr, "NAXIS1",
        "image width", "%d", movie->header->uiImageWidth);
    if (!keyword_added) goto keyword_fail;
    keyword_added = FITSHeaderAdd(hdr, "NAXIS2",
        "image height", "%d", movie->header->uiImageHeight);
    if (!keyword_added) goto keyword_fail;
    if (!is_mono) {
        keyword_added = FITSHeaderAdd(hdr, "NAXIS3",
            "channels", "%d", 3);
        if (!keyword_added) goto keyword_fail;
    }
    if (is_mono && color_id > COLOR_MONO) {
        /* Bayer pattern */
        char bayer_pat[BUFLEN];
        bayer_pat[0] = '\0';
        sprintf(bayer_pat, "'%s    '", SERGetColorString(color_id));
        keyword_added = FITSHeaderAdd(hdr, "BAYERPAT",
            "Bayer color pattern", "%s", bayer_pat);
        if (!keyword_added) goto keyword_fail;
    }
    if (SERMovieHasTrailer(movie)) {
        uint32_t usec = 0;
        time_t frame_time = 0;
        char timestamp[24];
        size_t datelen = 0;
        uint64_t frame_datetime = SERGetFrameDate(movie, frame_idx);
        if (frame_datetime > 0)
            frame_time = SERVideoTimeToUnixtime(frame_datetime, &usec);
        if (frame_time > 0) {
            struct tm *frame_tm = gmtime(&frame_time);
            datelen = strftime(timestamp, 24, "%Y-%m-%dT%H:%M:%S", frame_tm);
        }
        if (datelen == 19) {
            char *ptr = timestamp + datelen;
            usec /= 1000;
            if (usec < 1000) datelen += snprintf(ptr, 5, ".%03d", usec);
            else SERLogWarn(LOG_TAG_WARN "Invalid microsec. for frame date\n");
        }
        if (datelen == 23) {
            timestamp[23] = '\0';
            keyword_added = FITSHeaderAdd(hdr, "DATE-OBS",
                "UTC date of observation", "'%s'", timestamp);
            if (!keyword_added) goto keyword_fail;
        }
    }
    /* End header */
    if (!FITSHeaderEnd(hdr)) goto keyword_fail;
    data_unit = FITSCreateDataUnit(pixels, size, &data_size);
    if (data_unit == NULL) {
        SERLogErr(LOG_TAG_ERR "Failed to create FITS data unit\n");
        goto fail;
    }
    size_t nwritten = 0, totwritten = 0, remain = hdr->size;
    char *ptr = (char *) hdr->header;
    assert(hdr->header != NULL);
    SERLogInfo("FITS Header: added %d keyword(s)\n", hdr->count);
    printf("Writing %zu bytes of FITS header\n", hdr->size);
    while (totwritten < hdr->size) {
        nwritten = fwrite(ptr, 1, remain, imagefile);
        if (nwritten <= 0) break;
        totwritten += nwritten;
        ptr += nwritten;
        remain -= nwritten;
    }
    if (totwritten != hdr->size) {
        SERLogErr(LOG_TAG_ERR "Failed to write header unit to FITS file\n");
        goto fail;
    }
    nwritten = 0, totwritten = 0, remain = data_size;
    ptr = (char *) data_unit;
    printf("Writing %zu bytes of FITS data\n", data_size);
    while (totwritten < data_size) {
        nwritten = fwrite(ptr, 1, remain, imagefile);
        if (nwritten <= 0) break;
        totwritten += nwritten;
        ptr += nwritten;
        remain -= nwritten;
    }
    if (totwritten != data_size) {
        SERLogErr(LOG_TAG_ERR "Failed to write data unit to FITS file\n");
        goto fail;
    }
    FITSReleaseHeaderUnit(hdr);
    free(data_unit);
    return 1;
keyword_fail:
    SERLogErr(LOG_TAG_ERR "Failed to add FITS keyword\n");
fail:
    if (hdr != NULL) FITSReleaseHeaderUnit(hdr);
    if (data_unit != NULL) free(data_unit);
    return 0;
}

static int saveFrame(SERMovie *movie, int frame_id) {
    uint32_t frame_idx = 0;
    char *err = NULL;
    char errmsg[BUFLEN];
    void *pixels = NULL;
    FILE *imagefile = NULL;
    int format = conf.image_format;
    if (format == 0) format = IMAGE_FORMAT_RAW;
    if (frame_id == 0) {
        err = "invalid frame id: 0";
        goto fail;
    }
    if (frame_id < 0) frame_idx = SERGetFrameCount(movie) + frame_id;
    else frame_idx = frame_id - 1;
    if (frame_idx >= SERGetFrameCount(movie)) {
        sprintf(errmsg, "frame id %d beyond movie frames %d", frame_idx + 1,
            SERGetFrameCount(movie));
        err = errmsg;
        goto fail;
    }
    size_t size = 0;
    int big_endian = IS_BIG_ENDIAN;
    if (format == IMAGE_FORMAT_FITS) big_endian = 1;
    pixels = SERGetFramePixels(movie, frame_idx, big_endian, &size);
    if (pixels == NULL || size == 0) {
        sprintf(errmsg, "could not get frame %d pixels", frame_id);
        err = errmsg;
        goto fail;
    }
    SERLogInfo("Read %zu pixel byte(s)\n", size);
    char *dir = conf.output_dir;
    char outpath[PATH_MAX];
    char suffix[BUFLEN];
    char *ext = NULL;
    outpath[0] = '\0';
    suffix[0] = '\0';
    snprintf(suffix, BUFLEN, "-frame-%d", frame_idx + 1);
    if (dir == NULL) dir = "/tmp";
    if (format == IMAGE_FORMAT_FITS) ext = ".fit";
    else if (format == IMAGE_FORMAT_RAW) ext = ".raw";
    else {
        SERLogErr(LOG_TAG_ERR, "Invalid image format\n");
        goto fail;
    }
    if (!makeFilepath(outpath, movie->filepath, dir, suffix, ext)) {
        SERLogErr("Failed to create temporary filepath\n");
        goto fail;
    }
    if (fileExists(outpath) && !conf.overwrite) {
        int overwrite = askForFileOverwrite(outpath);
        if (!overwrite) goto fail;
    }
    imagefile = fopen(outpath, "w");
    if (imagefile == NULL) {
        SERLogErr(LOG_TAG_ERR "Could not open '%s' for writing\n", outpath);
        goto fail;
    }
    if (format == IMAGE_FORMAT_FITS) {
        if (!saveFITSImage(movie, imagefile, frame_idx, pixels, size)) {
            SERLogErr("Could not create FITS file\n");
            goto fail;
        }
    } else if (format == IMAGE_FORMAT_RAW) {
        SERLogInfo("Writing %zu bytes to raw image\n", size);
        size_t nwritten = 0, totwritten = 0, remain = size;
        char *ptr = pixels;
        while (totwritten < size) {
            nwritten = fwrite(pixels, 1, remain, imagefile);
            if (nwritten <= 0) break;
            totwritten += nwritten;
            ptr += totwritten;
            remain -= totwritten;
        }
        if (totwritten != size) {
            SERLogErr(LOG_TAG_ERR "Failed to write image\n");
            goto fail;
        }
    }
    free(pixels);
    fclose(imagefile);
    SERLogSuccess("Frame image saved to:\n'%s'\n", outpath);
    return 1;
fail:
    if (err != NULL) SERLogErr(LOG_TAG_ERR "%s\n", err);
    if (pixels != NULL) free(pixels);
    if (imagefile != NULL) fclose(imagefile);
    return 0;
}

static int fixMovie(SERMovie *movie) {
    if (movie->warnings == 0) {
        SERLogSuccess("This movie has no issues, no fix needed ;)\n");
        return 1;
    }
    if (movie->warnings & WARN_INCOMPLETE_FRAMES) {
        SERLogInfo("Trying to fix incomplete frames...\n");
        size_t frame_count = SERGetRealFrameCount(movie);
        if (frame_count == 0) {
            SERLogErr("Movie has no frames!\n");
            goto fail;
        }
        SERFrameRange range;
        range.from = 0;
        range.to = frame_count - 1;
        range.count = frame_count;
        char *output_path = conf.output_path;
        if (conf.use_winjupos_filename) output_path = NULL;
        if (!extractFramesFromVideo(movie, output_path, &range)) {
            SERLogErr("Failed to fix movie\n");
            goto fail;
        }
    }
    return 1;
fail:
    return 0;
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
    SERMovie *movie = SEROpenMovie(filepath);
    if (movie == NULL) {
        SERLogErr(LOG_TAG_ERR "Could not open movie at: '%s'\n", filepath);
        goto err;
    }
    movie->invert_endianness = conf.invert_endianness;
    printMovieInfo(movie);
    if (movie->warnings > 0 && !conf.do_check)
        printMovieWarnings(movie);
    int check_succeded = 1;
    if (conf.do_check) check_succeded = performMovieCheck(movie, NULL);
    SERHeader *header = movie->header;
    int action = conf.action;
    if (action == ACTION_FIX) {
        if (!fixMovie(movie)) goto err;
        goto final;
    }
    if (action < ACTION_SPLIT && action != ACTION_NONE && check_succeded) {
        /* Split or cut action */
        int from = conf.frames_from, to = conf.frames_to,
            count = conf.frames_count;
        SERFrameRange range;
        char *errmsg = NULL;
        if (!determineFrameRange(header, &range, from, to, count, &errmsg)) {
            SERLogErr(LOG_TAG_ERR "Invalid frame range: ");
            if (errmsg == NULL) errmsg = "could not determine frame range";
            SERLogErr("%s\n", errmsg);
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
        goto final;
    } else if (conf.action == ACTION_SPLIT && check_succeded) {
        if (!determineSplitRanges(movie)) {
            SERLogErr("Failed to split movie!\n");
            goto err;
        }
        if (!splitMovie(movie)) goto err;
    } else if (conf.action == ACTION_SAVE_FRAME) {
        if (!saveFrame(movie, conf.save_frame_id)) {
            SERLogErr("Failed to save frame\n");
            goto err;
        }
    }
    if (conf.log_to_json) {
        char json_filename[PATH_MAX + 1];
        makeFilepath(json_filename, filepath, "/tmp/", NULL, ".json");
        int do_log = 1;
        if (fileExists(json_filename) && !conf.overwrite)
            do_log = askForFileOverwrite(json_filename);
        if (do_log) {
            FILE *json = fopen(json_filename, "w");
            if (json == NULL) {
                SERLogErr(LOG_TAG_ERR "Could not open '%s' for writing!\n",
                    json_filename);
                return 1;
            }
            logToJSON(json, movie);
            printf("JSON saved to: '%s'\n", json_filename);
        }
    }
final:
    SERCloseMovie(movie);
    return 0;
err:
    SERCloseMovie(movie);
    return 1;
}
