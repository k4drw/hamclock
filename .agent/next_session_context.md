# HamClock Refactoring Context for Next Session

**Last Updated:** 2026-02-04
**Objective:** Continue removing dependencies on `clearskyinstitute.com` backend.

## Current Status
*   **SDO Images:** ✅ Fixed BMP write error. Now fully local/direct.
*   **Space Wx (SSN, Flux, DRAP, Aurora):** ✅ Fully local/direct.
*   **World Weather (Data Pane):** ✅ Fully local/direct.
*   **PSKP/RBN/Contests:** ✅ Fully local/direct.

## Remaining Backend Dependencies
The following features still rely on `backend_host` and are the priority for the next session:

1.  **VOACAP Propagation (High Priority / Hard)**
    *   **Files:** `wifi.cpp` (`retrieveBandConditions`), `mapmanage.cpp` (`installQueryMaps` -> `/fetchVOACAP...`).
    *   **Issue:** The VOACAP engine runs on the backend. We need to decide whether to port `voacapl` (C/Fortran) to run locally on the client (Pi/Linux) or find a public API.
    *   **Start with:** Investigating `fetchBandConditions.pl` usage and what `bc_matrix` needs.

2.  **Weather Map Isobars (High Priority)**
    *   **Files:** `mapmanage.cpp` (`installFileMaps` for `CM_WX`).
    *   **Issue:** The pressure isobar map (`map-D-Wx-mB.bmp`) is pre-generated on the backend.
    *   **Plan:** We are already fetching Open-Meteo data for the pane. We need to see if we can generate the isobar map locally using the same data or similar.

3.  **Static Map Assets (Medium Priority)**
    *   **Files:** `mapmanage.cpp` (`openMapFile`).
    *   **Issue:** Downloads missing static assets (Countries, Terrain, etc.) from backend.
    *   **Plan:** Ensure these are bundled in the repo or downloaded from a standard static host (GitHub Pages/S3) instead of the custom backend script.

## Completed Items (This Session)
*   **OTA Updates:** Refactored to use GitHub Releases (API + raw version.cpp). Removed dependency on `version.pl`.
*   **Release v4.23:** Tagged and released.

## Helpful Greps
*   `grep "backend_host" *.cpp` -> Shows all remaining touchpoints.
*   `grep "fetchVOACAP" *.cpp` -> Shows propagation map calls.

## Note
The `plotServerFile` function was removed in the previous session as it was dead code.
OTA functionality is now independent of the backend.
