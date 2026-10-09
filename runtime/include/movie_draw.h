// Movie playback (movie_draw.cpp).
#pragma once

#ifdef MH3_DEVELOPMENT
// The debug port's `movie skip` (game thread): the open movie's player sees
// it finished at its next check and closes it, as at its last frame. False
// when no movie is open.
bool SkipMovie();
#endif
