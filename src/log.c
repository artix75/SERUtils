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
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include "log.h"

int SERLogUseColors = 0;
int SERLogLevel = LOG_LEVEL_ERR;

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

void SERLog(int level, const char* format, ...) {
    if (level < SERLogLevel) return;
    FILE *out = (level > LOG_LEVEL_WARN ? stderr : stdout);
    int color = 0;
    if (SERLogUseColors) {
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

void SERPrintHeader(char *str) {
    size_t len = strlen(str), max_len = get_terminal_columns(),
           nspaces = 2, r, fill_len, i;
    assert(len < (max_len - nspaces));
    r = max_len - len;
    fill_len = (r - nspaces) / 2;
    if (SERLogUseColors) printf("\033[1m");
    for (i = 0; i < fill_len; i++) printf("=");
    for (i = 0; i < (nspaces / 2); i++) printf(" ");
    printf("%s", str);
    for (i = 0; i < (nspaces / 2); i++) printf(" ");
    for (i = 0; i < fill_len; i++) printf("=");
    if (SERLogUseColors) printf("\033[0m");
    printf("\n\n");
}

void SERPrintFieldValuePair(char *field, const char *field_format, ...) {
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

void SERLogProgress(char *what, int current, int tot) {
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
