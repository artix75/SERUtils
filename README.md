# SERUtils

SERUtils is both a command-line tool and a linkable dynamic library that can be used to parse and modify **SER** movie files.

SER movie format is mainly used for planetary and lunar imaging and it can be considered the *de facto* standard for this kind of astronomical imaging.

The format provides raw frame images, a header containing both astronomical metadata and info about the movie itself, and (optional) precise timestamps for each frame.

You can find SER format specifications at [http://www.grischa-hahn.homepage.t-online.de/astro/ser/SER%20Doc%20V3b.pdf](http://www.grischa-hahn.homepage.t-online.de/astro/ser/SER%20Doc%20V3b.pdf).

SERUtils is written in standard ANSI C.


## Supported Platforms

SERUtils has been developed and tested on POSIX platforms, so it works on both Linux and macOS/OS X.
Anyway, it should be easily portable to other kinds of platforms, since it doesn't heavily rely on POSIX API.

## Build and Install

To build it, just type:

`% make`

If you want to install it (both the command line utility and the library), type:

`% make install`

By default, SERUtils will be installed into /usr/local/

You can use make PREFIX=/some/other/directory install if you wish to use a different destination.

## Usage

### Command Line Utility

The command-line utility (named **serutils**) can be used to inspect a movie, extract or cut some frames from it, or even split the movie into multiple movies.
It can also be used to check movie errors and eventually fix them.

You can take a look at various actions and options by typing `--help`.

`% serutils --help`

Some example:

`% serutils my-movie.ser`

If you don't specify any option, serutils will just output some info about the movie, such as frame count, bits per pixel, color scheme, dates, metadata, and so on.

`% serutils --extract 10,20 my-movie.ser`

Extract 20 frames starting from frame 10.

`% serutils --extract 10..20 my-movie.ser`

Extract frames from 10 up to 20.

`% serutils --extract 50..-1 my-movie.ser`

Extract frames from 50 up to the last frame.

You can use the `--cut` option in the same fashion in order to cut (strip) frames from the movie option in a same fashion in order to cut frames from the movie.

You can split the movie into multiple movies by using the `--split` option, ie:

`% serutils --split 5   # Split movie into 5 movies` 

`% serutils --split 60s # Split movie into 60 seconds-long movies` 

`% serutils --split 200f # Split movie into 200 frames-long movies` 

### Library

Here's an example of a simple C program using the library:

``` C
/* ser_movie_example.c */

#include <stdio.h>
#include <serutils/serutils.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s SER_MOVIE_PATH\n", argv[0]);
        return 1;
    }
    char *moviefile = argv[1];
    SERMovie *movie = SEROpenMovie(moviefile);
    if (movie == NULL) {
        fprintf(stderr, "Unable to open movie: '%s'\n", moviefile);
        return 1;
    }
    uint32_t frame_count = SERGetFrameCount(movie);
    printf("Frame count: %d\n", frame_count);
    SERCloseMovie(movie);
    return 0;
}
```

In order to compile it with GCC:

`% gcc -o ser_movie_example -lserutils ser_movie_example.c`

Take a look at INSTALL_PREFIX_DIR/include/serutils/*.h header files in order to see which functions are available.
