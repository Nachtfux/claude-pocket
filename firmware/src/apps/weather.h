#pragma once

namespace apps {
namespace weather {

// Internet weather screen. On enter we IP-geolocate (ip-api.com, plain HTTP),
// then fetch current conditions and a 3-day forecast from Open-Meteo's free
// API (no key needed). Press `r` to refresh, `` ` `` to return to the
// launcher. Brand palette + animated Claude spark to stay visually
// consistent with the rest of the app.
void enter();
void tick();

}  // namespace weather
}  // namespace apps
