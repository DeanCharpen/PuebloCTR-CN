/* Stage 6.1 of the 3DS port: the movies through the New 3DS's H.264 decoder (MVD) in place of the CRI player.
 * game/sofdec.cpp (patch 0327) asks re4_movie_hw_open at startApp; when the platform plays the movie (its file
 * converted beforehand by PuebloPrep), the CRI handle stays NULL and appMain / draw /
 * finishMovie go through these. runtime/movie_status.cpp holds the table: NULL (Linux) - the CRI player as before;
 * port/whole/3ds/movie_3ds.cpp sets it. */
#ifndef RE4_MOVIE_HW_H
#define RE4_MOVIE_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Re4MovieHw {
    int (*open)(const char* fname);       /* the game's name ("movie/opening.sfd"); 1: it plays here */
    int (*step)(void);                    /* one game frame: frames decoded up to the sound's time; 0: the end */
    int (*shown)(void);                   /* a frame is there to show */
    void (*close)(int skipped);
    void (*time)(int* count, int* scale); /* the sound's time (as mwPlyGetTime) */
    void (*mark)(const void* texels);     /* the GX texture the game draws the frame with (its texels never read) */
} Re4MovieHw;

extern const Re4MovieHw* re4_movie_hw;

/* The game's side (runtime/movie_status.cpp) */
int re4_movie_hw_open(const char* fname);
int re4_movie_hw_step(int* mwstat);
void re4_movie_hw_close(int skipped);
void re4_movie_hw_mark(const void* texels);

/* The renderer's side (3DS, port/whole/3ds/movie_3ds.cpp): the texels the movie's GX texture names (NULL: none), and
 * the newest frame (MVD's output, 512x256 RGB565, top row first) with its number; 0: none */
extern const void* volatile re4_movie_3ds_mark;
int re4_movie_3ds_frame(const void** out, uint32_t* seq);

#ifdef __cplusplus
}
#endif

#endif
