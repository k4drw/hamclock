# HamClock Refactoring Progress Report

**Date:** 2026-01-31
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
| **Aurora (Stats)** | `/aurora/aurora.txt` | **NOAA HPI** (GW) | ✅ Complete (Native libcurl + Auto-scale) |

## 2. Infrastructure Improvements
*   **Native HTTPS**: Integrated `libcurl` to handle all external data fetches directly in C++, removing reliance on `system("wget")`.
*   **JSON Parsing**: Integrated `ArduinoJson` to robustly handle modern API responses.
*   **Data Bundling**: Added `data/cities2.txt` to the repository.
*   **Installation**: Updated `Makefile` to install `cities2.txt` to `/usr/local/share/hamclock/`.
*   **Self-Healing Cache**: Refactored `cachefile.cpp` to seed the cache from the bundled `cities2.txt` if the download fails (protecting new installs against proxy shutdown).
*   **Bug Fixes**: Fixed core dumps related to null/missing JSON fields in NOAA data.

## 3. Pending / Remaining Proxy Dependencies
The following items still rely on the proxy. They are prioritized for the next session.

| Feature | Proxy Path | Notes / Complexity | Priority |
| :--- | :--- | :--- | :--- |
| **DRAP Stats** | `/drap/stats.txt` | **Medium**. Direct fetch from NOAA (`drap_global.txt`) is possible, but NOAA only provides a *current snapshot*. Replicating the 24h history graph requires local history implementation. | Medium |
| **DRAP Map** | `/maps/...DRAP.bmp` | **Hard**. Currently downloads pre-rendered map images. Switching to direct requires implementing a local Map Generator (Text Grid -> Pixel Projection). | Phase 2 |
| **WSPR Spots** | `/fetchWSPR.pl` | **Low**. WSPRnet provides a JSON API. Refactor `pskreporter.cpp` to parse native JSON. | High |
| **PSKReporter** | `/fetchPSK...` | **Medium**. Source is proper XML. Requires implementing XML parsing logic in C++ to replace Proxy's format conversion. | Medium |
| **RBN Spots** | `/fetchRBN.pl` | **High**. Source is Telnet Stream. Requires writing a complex Telnet client with stream filtering to replace the Proxy's aggregation logic. | Phase 2 |
| **VOACAP** | `/fetchVOACAP...` | **Very Hard**. Server-side Fortran engine. Moving client-side requires porting VOACAP or using a public API (scarce). | Phase 3 |

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

## 5. Technical Implementation Notes (Critical for Future Devs)
*   **JSON Parsing:**
    *   **Buffer Size**: `ArduinoJson` requires significant overhead. We increased `DynamicJsonDocument` buffers to **1MB** (from 200KB) to prevent `NoMemory` errors with NOAA's ~100KB files. **Do not reduce this.**
    *   **Timestamps**: NOAA JSONs use inconsistent formats. Some use `YYYY-MM-DD HH:MM:SS`, others `YYYY-MM-DDTHH:MM:SSZ`. The parser in `spacewx.cpp` (`parse_noaa_json_time`) uses `%*c` to handle both separators.
*   **Data Quirks:**
    *   **BzBt**: The `mag-1-day.json` file header can be misleading. We confirmed via testing that **Bt (Total Field)** is at index **6** (not 4).
    *   **Aurora**: We switched from "Aurora Chances" (%) to "Hemispheric Power Index" (GW). The graph now **auto-scales** (0-max) instead of fixed 0-100.
*   **Network:**
    *   **Libcurl**: We replaced `system("wget...")` with a native `curlDownload` wrapper (in `src/hal/linux/System.cpp`) linking against `libcurl`. This provides robust HTTPS, timeout control, and error logging. Do not revert to `WiFiClientSecure` for these external fetches without verifying TLS compatibility.

## 6. Next Steps
*   Tackle **DRAP** and **Map Overlays** (Phase 2).
*   Investigate local `voacapl` integration (Phase 2).
*   **Push master branch to GitHub.**
