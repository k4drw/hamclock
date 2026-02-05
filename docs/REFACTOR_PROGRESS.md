# HamClock Refactoring Progress Report

**Date:** 2026-02-03
**Objective:** Remove dependencies on the ClearSky Institute proxy server (`clearskyinstitute.com`) before its scheduled shutdown in June 2026.
**Strategy:** Replace proxied data fetches with direct API calls to source providers (NOAA, Open-Meteo, etc.) or internal logic.

## 1. Completed Refactors (Direct Connect)
The following features no longer use the proxy. They connect directly to the primary data source or use local resources.

| Feature | Old Proxy Path | New Source / Implementation | Status |
| :--- | :--- | :--- | :--- |
| **IP Geolocation** | `/fetchIPGeoloc.pl` | **ip-api.com** (JSON) | ✅ Complete |
| **Weather Data** | `/wx.pl` | **Open-Meteo API** (JSON) | ✅ Complete |
| **City Names** | (via `/wx.pl`) | **Internal Database** (`cities2.txt`) | ✅ Complete (Offline/Self-contained) |
| **Solar Flux (SFI)** | `/solar-flux/...` | **NOAA SWPC** (`10cm-flux-30-day.json`) | ✅ Complete |
| **Sunspots (SSN)** | `/ssn/ssn-31.txt` | **NOAA SWPC** (`daily-solar-indices.txt`) | ✅ Complete |
| **Planetary Kp** | `/geomag/kindex.txt` | **NOAA SWPC** (`noaa-planetary-k-index-forecast.json`) | ✅ Complete |
| **Solar Wind** | `/solar-wind/...` | **NOAA SWPC** (`plasma-1-day.json`) | ✅ Complete |
| **Bz / Bt** | `/Bz/Bz.txt` | **NOAA SWPC** (`mag-1-day.json`) | ✅ Complete |
| **X-Ray Flux** | `/xray/xray.txt` | **NOAA SWPC** (`xrays-1-day.json`) | ✅ Complete |
| **Contests** | `/contests/...` | **WA7BNM / Google** (Direct iCal feed) | ✅ Complete |
| **Satellites** | `/esats/esats.txt` | **CelesTrak** (HTTPS) | ✅ Complete (Native libcurl) |
| **Aurora (Stats)** | `/aurora/aurora.txt` | **NOAA Ovation** (JSON) | ✅ Complete (Native libcurl + Max Grid %) |
| **WSPR Spots** | `/fetchWSPR.pl` | **wspr.live** (SQL API) | ✅ Complete (Direct SQL, native CSV parsing) |
| **DRAP (Stats)** | `/drap/stats.txt` | **NOAA Global** (Text) | ✅ Complete (Native libcurl + Max Grid Freq) |
| **PSKReporter** | `/fetchPSK...` | **retrieve.pskreporter.info** (XML) | ✅ Complete (Native XML parsing) |
| **RBN Spots** | `/fetchRBN.pl` | **telnet.reversebeacon.net** (Telnet) | ✅ Complete (Native Telnet Client) |
| **RSS Feeds** | `/RSS/web15rss.pl` | **arnewsline.org**, **hamweekly.com**, **ng3k.com** | ✅ Complete (Native Parsing) |


## 2. Infrastructure Improvements
*   **Native HTTPS**: Integrated `libcurl` to handle all external data fetches directly in C++, removing reliance on `system("wget")`.
*   **JSON Parsing**: Integrated `ArduinoJson` to robustly handle modern API responses.
*   **Data Bundling**: Added `data/cities2.txt` to the repository.
*   **Installation**: Updated `Makefile` to install `cities2.txt` to `/usr/local/share/hamclock/`.
*   **Self-Healing Cache**: Refactored `cachefile.cpp` to seed the cache from the bundled `cities2.txt` if the download fails (protecting new installs against proxy shutdown).
*   **Bug Fixes**: Fixed core dumps related to null/missing JSON fields in NOAA data.
*   **OTA Updates**: Transitioned OTA update mechanism to use GitHub Releases. The check logic was split into a lightweight polling of `version.cpp` on the master branch, and a full API query for release notes and assets when the user initiates an update.

## 3. Pending / Remaining Proxy Dependencies
The following items still rely on the proxy. They are prioritized for the next session.

| Feature | Proxy Path | Notes / Complexity | Priority |
| :--- | :--- | :--- | :--- |

| **DRAP Map** | `/maps/...DRAP.bmp` | **Hard**. Currently downloads pre-rendered map images. Switching to direct requires implementing a local Map Generator (Text Grid -> Pixel Projection). | Phase 2 |
| **PSKReporter** | `/fetchPSK...` | **retrieve.pskreporter.info** (XML) | ✅ Complete (Native XML parsing) |
| **RBN Spots** | `/fetchRBN.pl` | **telnet.reversebeacon.net** (Telnet) | ✅ Complete (Native Telnet Client) |
| **VOACAP** | `/fetchVOACAP...` | **Very Hard**. Server-side Fortran engine. Moving client-side requires porting VOACAP or using a public API (scarce). | Phase 3 |

| **On The Air** | `/ONTA/onta.txt` | **Medium**. Requires finding original source API (likely partial aggregation). | Pending |
| **Band Conditions** | `/fetchBandConditions.pl` | **Very Hard**. Part of VOACAP ecosystem. | Phase 3 |
| **DX Peditions** | `/dxpeds/dxpeditions.txt` | **Direct Fetch** of NG3K (HTML) + Local Parse | ✅ Complete |
| **SDO Images** | `/sdo_*.bmp` | **Direct Fetch** of NASA SDO (HTTPS) + Local Resize | ✅ Complete |


## 4. Verification Steps
1.  **Build**: `sudo make install` (installs binary and `cities2.txt`).
2.  **Run**: `./hamclock-runner.sh` (or `hamclock-800x480` etc).
3.  **Check Weather**:
    *   Verify weather icon and temperature appear.
    *   Verify City Name is correct (e.g., "London" not "DX").
4.  **Check Space Wx**:
    *   Verify SFI, SSN, Kp numbers are populated.
    *   Verify Solar Wind, Bz, and X-Ray graphs are plotting history.
5.  **Check Contests**:
    *   Open Contest pane, verify list is populated from WA7BNM.
6.  **Check SDO**:
    *   Open SDO pane, verify images are downloaded and displayed correctly.
7.  **Check DX Peditions**:
    *   Open DX Cluster pane / view, verify upcoming DXpeditions list.

## 5. Technical Implementation Notes (Critical for Future Devs)
*   **JSON Parsing:**
    *   **Buffer Size**: `ArduinoJson` requires significant overhead. We increased `DynamicJsonDocument` buffers to **1MB** (from 200KB) to prevent `NoMemory` errors with NOAA's ~100KB files. **Do not reduce this.**
    *   **Timestamps**: NOAA JSONs use inconsistent formats. Some use `YYYY-MM-DD HH:MM:SS`, others `YYYY-MM-DDTHH:MM:SSZ`. The parser in `spacewx.cpp` (`parse_noaa_json_time`) uses `%*c` to handle both separators.
*   **Data Quirks:**
    *   **BzBt**: The `mag-1-day.json` file header can be misleading. We confirmed via testing that **Bt (Total Field)** is at index **6** (not 4).
    *   **Aurora**: We switched to **NOAA Ovation** JSON grid. Since this source provides a probability grid (0-100%) but no history, we calculate the **Maximum Probability** in the grid and maintain a local history cache. We also added file persistence (`aurora_history.txt`) to restore the graph after a restart. This restores the "%" unit used in the original HamClock.
    *   **DRAP**: Similar to Aurora, we now fetch the **NOAA Global D-Region Absorption Prediction** text file (`drap_global_frequencies.txt`). This file provides a snapshot of absorption frequencies across a lat/lon grid. We parse this to find the global **Maximum Frequency**, and since it lacks history, we maintain a local history cache with file persistence (`drap_history.txt`).
*   **Network:**
    *   **Libcurl**: We replaced `system("wget...")` with a native `curlDownload` wrapper (in `src/hal/linux/System.cpp`) linking against `libcurl`. This provides robust HTTPS, timeout control, and error logging. Do not revert to `WiFiClientSecure` for these external fetches without verifying TLS compatibility.
    *   **URL Encoding**: WSPR queries require strict URL encoding (e.g., `%20` for spaces, `%27` for quotes). We added `urlencode.h` to handle this robustly, replacing manual loops.
*   **SDO Images:**
    *   **BMP Writing:** The `writeBMP565File` function uses `fopenOurs` which always prepends `our_dir`. Therefore, use relative filenames when calling it, to avoid `path/to/path/to/file` errors.
*   **DX Peditions:**
    *   **Parsed**: We download the raw NG3K HTML page and perform a lightweight parse in `dxpeds.cpp` to extract upcoming expeditions, mimicking the Perl script logic on the client.
    *   **Storage**: Cached in `dxpeditions.txt` in the user data directory.
*   **World Weather Grid:**
    *   **Native Generation**: The logic to generate the global weather grid (`wx.txt`) has been ported to C++ in `wx.cpp`. It fetches current data for a grid of points from **Open-Meteo**, replacing the need for a backend script or the prototype `gen_wx_grid.py`.
*   **OTA Updates:**
    *   **Versioning**: HamClock now checks `https://raw.githubusercontent.com/k4drw/hamclock/master/version.cpp` for the latest version string. This is a lightweight check to avoid hitting GitHub API rate limits on every poll.
    *   **Release Notes**: If a new version is detected, the full update process fetches `https://api.github.com/repos/k4drw/hamclock/releases/latest` to retrieve the release body (changelog) and the asset download URL.
    *   **Dynamic Unzip**: The update process no longer assumes the zip file matches the internal directory name. It unzips the release asset and dynamically detects the created directory (e.g., `hamclock-4.23`).

## 6. Next Steps
*   **Phase 2**: Tackle **VOACAP** replacement (likely requires local computation or new API).
*   **Phase 2**: Address **Static Map Assets** and **Weather Map** backgrounds.
*   **Phase 3**: ✅ Finalize **Updates / Diags** which inherently rely on a central server.
*   **Push master branch to GitHub.**
