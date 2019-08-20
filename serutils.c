#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#pragma pack(1)
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

int parseOptions(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] == '-') {
            if (*(++arg) == '-') arg++;
        } else break;
    }
    return i;
}

int parseHeader(FILE *video, SERHeader *header) {
    int ok = 1;
    fread((void *)&header, sizeof(SERHeader), 1, video);
    return ok;
}

int main(int argc, char **argv) {
    int filename_idx = parseOptions(argc, argv);
    assert(filename_idx < argc);
    char *filename = argv[filename_idx];
    FILE *video = fopen(filename, "r");
    if (video == NULL) {
        fprintf(stderr, "Could not open video file at: '%s'\n", filename);
        return 1;
    }
    fclose(video);
    SERHeader header;
    if (!parseHeader(video, &header)) {
        fprintf(stderr, "Could not parse header.\n");
        return 1;
    }
    return 0;
}