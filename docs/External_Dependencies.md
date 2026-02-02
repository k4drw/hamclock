# HamClock External Data Dependencies

HamClock relies heavily on a backend server (`clearskyinstitute.com`) to format and proxy data from various providers. If this server becomes unavailable, the features listed below will fail.

## Proxied Dependencies (Critical)
These endpoints connect to `backend_host` (default: `clearskyinstitute.com:80`).

| Feature | Endpoint Path | Original Source (Speculated) | Purpose of Proxy |
| :--- | :--- | :--- | :--- |
| ~~**Weather**~~ | ~~`/wx.pl`~~ | ~~OpenWeatherMap / NWS~~ | ~~Strips JSON, hides API keys, formats as key=value text.~~ |
| **Grid Wx** | `/worldwx/wx.txt` | Global Models (GFS?) | Generates global weather grid for map overlay. |
| ~~**Satellites**~~ | ~~`/esats/esats.txt`~~ | ~~CelesTrak / Space-Track~~ | ~~Filters massive TLE lists; formats names; pre-selects ham sats.~~ |
| ~~**Space Wx**~~ | ~~`/drap/stats.txt`~~ | ~~NOAA SWPC~~ | ~~Formats DRAP (D-Region Absorption) data.~~ |
| ~~**Space Wx**~~ | ~~`/Bz/Bz.txt`~~ | ~~NOAA SWPC~~ | ~~Formats Bz/Bt magnetic field data.~~ |
| ~~**Space Wx**~~ | ~~`/solar-wind/swind-24hr.txt`~~ | ~~NOAA SWPC~~ | ~~Formats solar wind speed/density.~~ |
| ~~**Space Wx**~~ | ~~`/ssn/ssn-31.txt`~~ | ~~NOAA / SIDC~~ | ~~Sunspot numbers.~~ |
| ~~**Space Wx**~~ | ~~`/solar-flux/solarflux-99.txt`~~ | ~~NOAA / Penticton~~ | ~~10.7cm Solar Flux data.~~ |
| ~~**Space Wx**~~ | ~~`/geomag/kindex.txt`~~ | ~~NOAA SWPC~~ | ~~Planetary K-index.~~ |
| ~~**Space Wx**~~ | ~~`/xray/xray.txt`~~ | ~~NOAA SWPC (GOES)~~ | ~~X-Ray flux data.~~ |
| **Propagation** | `/fetchVOACAP-TOA.pl` | VOACAP (Engine) | Runs VOACAP prediction; returns map overlay. |
| **Propagation** | `/fetchVOACAPArea.pl` | VOACAP (Engine) | Runs VOACAP Area prediction; returns map overlay. |
| **Map Images** | `/maps/*.z` | Static Storage | Serves pre-compressed RGB565 bitmaps for background maps. |
| **Digimodes** | `/fetchPSKReporter.pl` | PSKReporter.info | Filters massive XML feed by Grid/Call; returns CSV. |
| ~~**Digimodes**~~ | ~~`/fetchWSPR.pl`~~ | ~~WSPRnet.org~~ | ~~Filters WSPR database spots.~~ |
| **Digimodes** | `/fetchRBN.pl` | Reverse Beacon Network | Filters RBN telnet/database spots. |
| ~~**Contests**~~ | ~~`/contests/contests311.txt`~~ | ~~ContestCalendar.com~~ | ~~Scrapes HTML/Calendar; formats as simple list.~~ |
| ~~**Geolocation**~~ | ~~`/fetchIPGeoloc.pl`~~ | ~~IP-API / MaxMind~~ | ~~IP-to-Lat/Lon/Grid lookup.~~ |
| **Updates** | `/ham/HamClock/ESPHamClock.zip` | Local Build System | binary software updates. |

## Direct Connect Dependencies (Safe)
These features connect directly to the source and should continue working.

| Feature | URL / Host | Notes |
| :--- | :--- | :--- |
| **Time** | `pool.ntp.org` | Standard NTP protocol. |
| **Solar Images** | `sdo.gsfc.nasa.gov` | Direct HTTPS fetch of JPEG images. |
| **DX Cluster** | User Configured | Telnet connection to DX Spider/AR-Cluster nodes. |
| **Contest Links** | `contestcalendar.com` | Hyperlinks open in browser (but data is proxied). |
| **Geolocation** | `ip-api.com` | JSON IP Geolocation. |
| **Contests** | `calendar.google.com` | WA7BNM iCal Feed via Google. |
| **Weather** | `api.open-meteo.com` | JSON Forecast API (Free, No Key). |
| **City Data** | Local / GeoNames | Bundled `cities2.txt` (Self-updating). |
| **Satellites** | `celestrak.org` | HTTPS GP TLE Feed (Amateur Group) (via libcurl). |
| **WSPR Spots** | `db1.wspr.live` | Direct SQL via HTTP (ClickHouse DB) (via libcurl). |
| **Aurora** | `noaa.gov` | Ovation Aurora Forecast (JSON Grid) (via libcurl). |
| **DRAP Stats** | `noaa.gov` | Global D-Region Absorption (Text Grid) (via libcurl). |
| **SFI Data** | `noaa.gov` | JSON 10cm Flux History (30-day) (via libcurl). |
| **Kp Data** | `noaa.gov` | JSON Planetary K-index Forecast/History (via libcurl). |
| **SSN Data** | `noaa.gov` | Daily Solar Indices (Text) (via libcurl). |
| **Solar Wind** | `noaa.gov` | JSON Plasma/Magnetic Data (1-day) (via libcurl). |


