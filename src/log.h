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

#ifndef __SER_LOG_H__
#define __SER_LOG_H__

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

#define logInfo(...) SERLog(LOG_LEVEL_INFO, __VA_ARGS__)
#define logNotice(...) SERLog(LOG_LEVEL_NOTICE, __VA_ARGS__)
#define logSuccess(...) SERLog(LOG_LEVEL_SUCCESS, __VA_ARGS__)
#define logWarn(...) SERLog(LOG_LEVEL_WARN, __VA_ARGS__)
#define logErr(...) SERLog(LOG_LEVEL_ERR, __VA_ARGS__)

extern int SERLogUseColors;
extern int SERLogLevel;

void SERLog(int level, const char* format, ...);
void SERPrintHeader(char *str);
void SERLogProgress(char *what, int current, int tot);

#endif /*__SER_LOG_H__ */
