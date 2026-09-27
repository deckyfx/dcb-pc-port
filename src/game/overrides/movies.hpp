#pragma once
// Native movie playback (see movies.cpp): the state the host loop and the override share.

#include "movie.hpp"

#include <string>
#include <vector>

namespace dcb {

struct MovieHost {
    platform::MoviePlayer player;
    bool active = false;  ///< a native movie is playing: present it instead of the game's picture
    bool skip = false;    ///< the player asked to skip (any key / Start)
    int index = -1;
};

MovieHost& movie_host();

/// Where movies are looked up (`movie/movie<N>.mpg`), in mount order: later entries shadow
/// earlier ones (a loose folder overrides a pack). Non-existent paths are skipped.
void attach_movies(const std::vector<std::string>& mounts);

}  // namespace dcb
