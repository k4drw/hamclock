/* manage space weather stats.
 */

#include "HamClock.h"
#include <ArduinoJson.h>
#include <sys/stat.h>

// retrieve pages
// retrieve pages -- all direct fetches now

// caches
static BzBtData bzbt_cache;
static SolarWindData sw_cache;
static SunSpotData ssn_cache;
static SolarFluxData sf_cache;
static DRAPData drap_cache;
static XRayData xray_cache;
static KpData kp_cache;
static NOAASpaceWxData noaasw_cache = {0, false, {'R', 'S', 'G'}, {}};
static AuroraData aurora_cache;
static DSTData dst_cache;

#define X(a, b, c, d, e, f, g, h, i) \
    {a, b, c, d, e, f, g, h, i},  // expands SPCWX_DATA to each array initialization in {}
SpaceWeather_t space_wx[SPCWX_N] = {SPCWX_DATA};
#undef X

/* bit mask of user's NXDXF_b SPCWX_t, or Auto (ie, sort based on impact) if
 * none. N.B. user selection relies on ranks being set
 */
static uint32_t spcwx_chmask;
#define SPCWX_AUTO 0  // spcwx_chmask value that means sort based on impact

// handy conversion from space_wx value to it ranking contribution
/* compute rank from space weather value
 */
static int computeSWRank(const SpaceWeather_t* sp) {
    return ((int)roundf((sp->a * sp->value + sp->b) * sp->value + sp->c));
}

/* qsort-style function to compare the scaled value of two SpaceWeather_t.
 * N.B. largest first, any bad values at the end
 */
static int swQSF(const void* v1, const void* v2) {
    const SpaceWeather_t* s1 = (SpaceWeather_t*)v1;
    const SpaceWeather_t* s2 = (SpaceWeather_t*)v2;

    if (!s1->value_ok)
        return (s2->value_ok);
    else if (!s2->value_ok)
        return (-1);

    int rank_val1 = computeSWRank(s1);
    int rank_val2 = computeSWRank(s2);
    return (rank_val2 - rank_val1);
}

/* init the slope and intercept of each space wx stat from sw_rank_page.
 * return whether successful.
 */
/* init the slope and intercept of each space wx stat.
 * Using hardcoded coefficients to avoid dependency on proxy.
 * return whether successful (always true now).
 */
static bool initSWFit(void) {
    // a, b, c coefficients for ranking
    // Order matches SPCWX_DATA in HamClock.h
    static const float coeffs[SPCWX_N][3] = {
        {0, 0.05, -6},    // 0: SSN
        {0, 1e6, -2},     // 1: X-Ray
        {0, 0.1, -15},    // 2: SFI
        {0, 3.2, -8.8},   // 3: Kp
        {0, 1, -2},       // 4: Solar Wind
        {0, 1, -20},      // 5: DRAP
        {0, -0.8, -2},    // 6: Bz
        {0, 3, -3},       // 7: NOAA SpW
        {0, 0.16, -6},    // 8: Aurora
        {-0.04, -0.2, 3}  // 9: DST
    };

    Serial.println("RANKSW: Using hardcoded coefficients");
    Serial.println("RANKSW:   Coeffs Name       a       b       c");

    for (int i = 0; i < SPCWX_N; i++) {
        SpaceWeather_t& sw_i = space_wx[i];
        sw_i.a = coeffs[i][0];
        sw_i.b = coeffs[i][1];
        sw_i.c = coeffs[i][2];
        Serial.printf("RANKSW: %13s %7g %7g %7g\n", plot_names[sw_i.pc], sw_i.a, sw_i.b, sw_i.c);
    }

    return true;
}

/* go through space_wx and set the rank according to the importance of each
 * value.
 */
static void sortSpaceWx() {
    // copy space_wx and sort best first
    SpaceWeather_t sw_sort[SPCWX_N];
    memcpy(sw_sort, space_wx, sizeof(sw_sort));
    qsort(sw_sort, SPCWX_N, sizeof(SpaceWeather_t), swQSF);

    // set and log rank of each entry, 0 is best
    Serial.println("RANKSW: rank      name    value score");
    for (int i = 0; i < SPCWX_N; i++) {
        SPCWX_t sp_i = sw_sort[i].sp;
        SpaceWeather_t& sw_i = space_wx[sp_i];
        sw_i.rank = i;
        Serial.printf("RANKSW: %d %12s %8.2g %3d\n", i, plot_names[sw_i.pc], sw_i.value, computeSWRank(&sw_i));
    }
}

/* present menu of all Space Weather choices in NCDXF_b, allow op to choose up
 * to four or Auto.
 */
static void runNCDXFSpcWxMenu(void) {
    // build menu of all SPCWX_N plus gap plus Auto
    MenuItem mitems[SPCWX_N + 2];
    for (int i = 0; i < SPCWX_N; i++)
        mitems[i] = {MENU_0OFN, (spcwx_chmask & (1 << i)) != 0, 1, 1, space_wx[i].name, 0};
    mitems[SPCWX_N] = {MENU_BLANK, 0, 0, 0, 0, 0};
    mitems[SPCWX_N + 1] = {MENU_TOGGLE, spcwx_chmask == SPCWX_AUTO, 2, 1, "Auto", 0};

    // its box
    SBox menu_b = NCDXF_b;  // copy, not ref!
    menu_b.x += 1;
    menu_b.w = 0;  // shrink wrap

    // run
    SBox ok_b;
    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_NOCANCEL, 1, NARRAY(mitems), mitems};
    if (runMenu(menu)) {
        // set mask bits and ascending rank to implement chosen params unless
        // explicit Auto
        if (mitems[SPCWX_N + 1].set) {
            Serial.printf("SPCWX: NCDXF table is now Auto\n");
            spcwx_chmask = SPCWX_AUTO;
            sortSpaceWx();
        } else {
            // N.B. assign ranks in same order as initSpaceWX()
            spcwx_chmask = 0;
            int rank = 0;
            for (int i = 0; i < SPCWX_N; i++) {
                if (mitems[i].set) {
                    space_wx[i].rank = rank++;
                    spcwx_chmask |= (1 << i);
                } else {
                    space_wx[i].rank = SPCWX_N;  // impossible rank
                }
            }
            if (rank > NCDXF_B_NFIELDS)
                Serial.printf("SPCWX: NDXCF table using only first %d selections\n", NCDXF_B_NFIELDS);
        }

        // save mask
        NVWriteUInt32(NV_SPCWXCHOICE, spcwx_chmask);
        Serial.printf("SPCWX: choice mask 0x%08x\n", spcwx_chmask);

        // refresh box
        drawNCDXFSpcWxStats(RA8875_BLACK);
    }
}

/* handle touch location s known to be within NCDXF_b showing space weather
 * stats. if within a numeric value: insure the space stat is in a visible Pane.
 * else in parameter name:    offer menu of desired parameters or Auto
 * N.B. coordinate layout with drawNCDXFStats()
 */
void doNCDXFSpcWxTouch(const SCoord& s) {
    // decide which row, counting down from 0
    const uint16_t y_top = s.y - NCDXF_b.y;
    const uint16_t h = NCDXF_b.h / NCDXF_B_NFIELDS;
    const uint16_t r = y_top / h;

    // decide whether s is in number or name
    if (y_top > r * h + 3 * h / 4) {
        // s is in the thin title area so run menu to select stats or Auto
        runNCDXFSpcWxMenu();

    } else {
        // s is in larger name portion so engage data pane whose rank == row
        for (int i = 0; i < SPCWX_N; i++)
            if (space_wx[i].rank == r) setPlotVisible(space_wx[i].pc);
    }
}

/* draw the NCDXF_B_NFIELDS highest ranking space_wx in NCDXF_b.
 * use the given color for everything unless black then use the associated pane
 * colors.
 */
void drawNCDXFSpcWxStats(uint16_t color) {
    // handy
    static const char err[] = "Err";

    // arrays for drawNCDXFStats()
    char titles[NCDXF_B_NFIELDS][NCDXF_B_MAXLEN];
    char values[NCDXF_B_NFIELDS][NCDXF_B_MAXLEN];
    uint16_t colors[NCDXF_B_NFIELDS];

    // assign by rank down from top, 0 first
    for (int i = 0; i < NCDXF_B_NFIELDS; i++) {
        // reset in case not all used
        titles[i][0] = values[i][0] = '\0';
        for (int j = 0; j < SPCWX_N; j++) {
            if (space_wx[j].rank == i) {
                // set title i for param j, using common error else per-param value

                strcpy(titles[i], space_wx[j].name);
                if (!space_wx[j].value_ok) {
                    strcpy(values[i], err);
                    colors[i] = RA8875_RED;
                } else {
                    switch ((SPCWX_t)j) {
                        case SPCWX_SSN:
                            snprintf(values[i], sizeof(values[i]), "%.0f", space_wx[SPCWX_SSN].value);
                            colors[i] = SSN_COLOR;
                            break;

                        case SPCWX_XRAY:
                            xrayLevel(values[i], space_wx[SPCWX_XRAY]);
                            colors[i] = RGB565(255, 134, 0);  // XRAY_LCOLOR is too alarming
                            break;

                        case SPCWX_FLUX:
                            snprintf(values[i], sizeof(values[i]), "%.0f", space_wx[SPCWX_FLUX].value);
                            colors[i] = SFLUX_COLOR;
                            break;

                        case SPCWX_KP:
                            snprintf(values[i], sizeof(values[i]), "%.1f", space_wx[SPCWX_KP].value);
                            colors[i] = KP_COLOR;
                            break;

                        case SPCWX_SOLWIND:
                            snprintf(values[i], sizeof(values[i]), "%.1f", space_wx[SPCWX_SOLWIND].value);
                            colors[i] = SWIND_COLOR;
                            break;

                        case SPCWX_DRAP:
                            snprintf(values[i], sizeof(values[i]), "%.0f", space_wx[SPCWX_DRAP].value);
                            colors[i] = DRAPPLOT_COLOR;
                            break;

                        case SPCWX_BZ:
                            snprintf(values[i], sizeof(values[i]), "%.1f", space_wx[SPCWX_BZ].value);
                            colors[i] = BZBT_BZCOLOR;
                            break;

                        case SPCWX_NOAASPW:
                            snprintf(values[i], sizeof(values[i]), "%.0f", space_wx[SPCWX_NOAASPW].value);
                            colors[i] = NOAASPW_COLOR;
                            break;

                        case SPCWX_AURORA:
                            snprintf(values[i], sizeof(values[i]), "%.0f", space_wx[SPCWX_AURORA].value);
                            colors[i] = AURORA_COLOR;
                            break;

                        case SPCWX_DST:
                            snprintf(values[i], sizeof(values[i]), "%.0f", space_wx[SPCWX_DST].value);
                            colors[i] = DST_COLOR;
                            break;

                        case SPCWX_N:
                            break;  // lint
                    }
                }
            }
        }
    }

    // do it
    drawNCDXFStats(color, titles, values, colors);
}

/* return the next time of routine download.
 */
time_t nextRetrieval(PlotChoice pc, int interval) {
    time_t next_update = myNow() + interval;
    int nm = millis() / 1000 + interval;
    Serial.printf("%s data now good for %d sec at %d\n", plot_names[pc], interval, nm);
    return (next_update);
}

/* parse NOAA JSON time tag "2026-01-31 16:35:00.000"
 */
static time_t parse_noaa_json_time(const char* str) {
    if (!str) return 0;
    int y, m, d, H, M, S;
    if (sscanf(str, "%d-%d-%d%*c%d:%d:%d", &y, &m, &d, &H, &M, &S) >= 6) {
        tmElements_t tm;
        tm.Year = y - 1970;
        tm.Month = m;
        tm.Day = d;
        tm.Hour = H;
        tm.Minute = M;
        tm.Second = S;
        return makeTime(tm);
    }
    return 0;
}

/* retrieve sun spot and SPCWX_SSN if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */
bool retrieveSunSpots(SunSpotData& ssn) {
    // check cache first
    if (myNow() < ssn_cache.next_update) {
        ssn = ssn_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/text/daily-solar-indices.txt";
    const char* tmp_fn = "/tmp/hc_ssn.txt";
    bool ok = false;

    // mark value as bad until proven otherwise
    space_wx[SPCWX_SSN].value_ok = false;
    ssn.data_ok = ssn_cache.data_ok = false;

    Serial.printf("SSN: %s\n", url);
    if (curlDownload(url, tmp_fn)) {
        updateClocks(false);
        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            char line[200];
            float vals[SSN_NV];
            int n_vals = 0;

            while (fgets(line, sizeof(line), fp)) {
                // Skip headers and comments
                if (line[0] == '#' || line[0] == ':' || strlen(line) < 10) continue;

                int y, m, d, flux, ssn_val;
                // Parse: Y M D Flux SSN ...
                if (sscanf(line, "%d %d %d %d %d", &y, &m, &d, &flux, &ssn_val) == 5) {
                    if (n_vals < SSN_NV) {
                        vals[n_vals++] = (float)ssn_val;
                    } else {
                        // shift left to keep newest at end
                        for (int k = 0; k < SSN_NV - 1; k++) vals[k] = vals[k + 1];
                        vals[SSN_NV - 1] = (float)ssn_val;
                    }
                }
            }
            fclose(fp);

            if (n_vals > 0) {
                // Calculate offset.
                // We want SSN_NV points. Array ends at "Today".
                // ssn_cache.x[i] = 1-SSN_NV + i;

                int offset = SSN_NV - n_vals;
                for (int i = 0; i < SSN_NV; i++) {
                    ssn_cache.x[i] = 1 - SSN_NV + i;

                    int src_i = i - offset;
                    if (src_i < 0)
                        ssn_cache.ssn[i] = vals[0];  // dupe oldest
                    else
                        ssn_cache.ssn[i] = vals[src_i];
                }

                space_wx[SPCWX_SSN].value = ssn_cache.ssn[SSN_NV - 1];
                space_wx[SPCWX_SSN].value_ok = true;
                ssn_cache.data_ok = true;
                ssn = ssn_cache;
                ok = true;
                Serial.printf("SSN: Last %.0f (count %d)\n", ssn_cache.ssn[SSN_NV - 1], n_vals);
            }
        }
    } else {
        Serial.printf("SSN: Download failed\n");
    }

    // set next update
    ssn_cache.next_update = ok ? nextRetrieval(PLOT_CH_SSN, SSN_INTERVAL) : nextWiFiRetry(PLOT_CH_SSN);
    return (ok);
}

/* return whether new SPCWX_SSN data are ready, even if bad.
 */
static bool checkForNewSunSpots(void) {
    if (myNow() < ssn_cache.next_update) return (false);

    SunSpotData ssn;
    return (retrieveSunSpots(ssn));
}

/* retrieve solar flux and SPCWX_FLUX if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */
/* retrieve solar flux and SPCWX_FLUX if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 * Uses NOAA SWPC JSON (HTTPS via wget)
 */
bool retrieveSolarFlux(SolarFluxData& sf) {
    // check cache first
    if (myNow() < sf_cache.next_update) {
        sf = sf_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/products/10cm-flux-30-day.json";
    const char* tmp_fn = "/tmp/hc_sfi.json";
    bool ok = false;

    // mark value as bad until proven otherwise
    space_wx[SPCWX_FLUX].value_ok = false;
    sf.data_ok = sf_cache.data_ok = false;

    Serial.printf("SFlux: %s\n", url);

    // Download
    if (curlDownload(url, tmp_fn)) {
        updateClocks(false);

        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            char* json_buf = (char*)malloc(fsize + 1);
            if (json_buf) {
                fread(json_buf, 1, fsize, fp);
                json_buf[fsize] = 0;

                StaticJsonDocument<8192> doc;  // NOAA 30-day is small, but just in case
                DeserializationError error = deserializeJson(doc, json_buf);

                if (!error && doc.is<JsonArray>()) {
                    JsonArray arr = doc.as<JsonArray>();
                    int noaa_count = arr.size();

                    // Reset cache x values (same formula as original)
                    for (int i = 0; i < SFLUX_NV; i++) {
                        sf_cache.x[i] = (i - (SFLUX_NV - 9 - 1)) / 3.0F;
                        sf_cache.sflux[i] = 0;
                    }

                    // NOAA data is TimeStr, Value. Oldest first.
                    // We fill backwards from "Now" (Index 89 in SFLUX_NV=99)
                    int cache_i = SFLUX_NV - 10;

                    // Iterate backwards through NOAA data (Newest first)
                    // Skip n=0 (header)
                    for (int n = noaa_count - 1; n >= 1 && cache_i >= 0; n--) {
                        JsonArray row = arr[n];
                        float flux = row[1].as<float>();

                        // Fill 3 slots for this day
                        for (int k = 0; k < 3 && cache_i >= 0; k++) {
                            sf_cache.sflux[cache_i--] = flux;
                        }
                    }

                    // Forward fill predictions from last known value
                    float last_val = sf_cache.sflux[SFLUX_NV - 10];
                    for (int i = SFLUX_NV - 9; i < SFLUX_NV; i++) {
                        sf_cache.sflux[i] = last_val;
                    }

                    // Success
                    space_wx[SPCWX_FLUX].value = last_val;
                    space_wx[SPCWX_FLUX].value_ok = true;
                    sf_cache.data_ok = true;
                    sf = sf_cache;
                    ok = true;

                    Serial.printf("SFlux: Last val %.0f\n", last_val);
                } else {
                    Serial.printf("SFlux: JSON Error %s\n", error.c_str());
                }
                free(json_buf);
            }
            fclose(fp);
        }
    } else {
        Serial.printf("SFlux: Download failed\n");
    }

    // set next update
    sf_cache.next_update = ok ? nextRetrieval(PLOT_CH_FLUX, SFLUX_INTERVAL) : nextWiFiRetry(PLOT_CH_FLUX);
    return (ok);
}

/* Update the local solarflux-history.txt cache with new monthly averages.
 * If cache is missing, seed it from the backend server first.
 * Then check spaceweather.gc.ca for new data.
 */
static bool updateSolarFluxHistory(void) {
    const char* cache_fn = "solarflux-history.txt";
    static time_t last_check = 0;

    // only check once a day
    if (myNow() - last_check < 86400) return true;
    last_check = myNow();

    // 1. Seed if missing
    FILE* fp = fopenOurs(cache_fn, "r");
    if (!fp) {
        // Try to copy from local static data file
        const char* bundles[] = {"data", "/usr/local/share/hamclock"};
        for (auto* b : bundles) {
            char static_seed[1024];
            snprintf(static_seed, sizeof(static_seed), "%s/%s", b, cache_fn);
            FILE* seed = fopen(static_seed, "r");
            if (seed) {
                Serial.printf("SFHist: Seeding cache from %s...\n", static_seed);
                fp = fopenOurs(cache_fn, "w");
                if (fp) {
                    char buf[1024];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), seed)) > 0) {
                        fwrite(buf, 1, n, fp);
                    }
                    fclose(fp);
                }
                fclose(seed);
                if (fp) break;  // found and copied
            }
        }

        // re-open read-only to verify
        fp = fopenOurs(cache_fn, "r");
        if (!fp) return false;
    }

    // 2. Determine last entry date in cache to see if we need update
    float last_frac_year = 0;
    char line[100];
    while (fgets(line, sizeof(line), fp)) {
        float fy, val;
        if (sscanf(line, "%f %f", &fy, &val) == 2) last_frac_year = fy;
    }
    fclose(fp);

    // Calculate "Last Month" fractional year
    // year + (month-1)/12.0
    tmElements_t tm;
    breakTime(myNow(), tm);
    // we want previous month
    int target_m = tm.Month - 1;  // 1-12 becomes 0-11
    int target_y = tm.Year + 1970;
    if (target_m < 1) {
        target_m = 12;
        target_y--;
    }

    // target fractional year for the COMPLETED previous month
    // e.g. if now is Feb 2026, target is Jan 2026.
    // Frac year = 2026 + (1-1)/12 = 2026.0
    float target_fy = target_y + (target_m - 1) / 12.0f;

    // simplistic epsilon check: if we have something close to or greater than target, we are good
    if (last_frac_year > target_fy - 0.001) {
        return true;
    }

    // 3. Fetch live data
    const char* src_url = "https://www.spaceweather.gc.ca/solar_flux_data/daily_flux_values/fluxtable.txt";
    const char* tmp_fn = "/tmp/flux_update.txt";

    Serial.printf("SFHist: Fetching update from %s\n", src_url);
    if (!curlDownload(src_url, tmp_fn)) return false;

    fp = fopen(tmp_fn, "r");
    if (!fp) return false;

    // 4. Parse and Compute Average
    // Looking for lines starting with Date/Time that match target YYYYMM
    char target_prefix[10];
    snprintf(target_prefix, sizeof(target_prefix), "%04d%02d", target_y, target_m);

    float sum_flux = 0;
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        // Skip headers (alpha chars)
        if (isalpha(line[0])) continue;

        // Line Fmt: DATE TIME JULIAN CAR_ROT OBS_FLUX ADJ_FLUX ...
        //           20251201 200000 2461011.385 2291.63 156.4 152.6 ...
        // We want col 5 (OBS_FLUX) or col 6 (ADJ_FLUX)?
        // Script uses $5. Let's count cols carefully.
        // 1:Date 2:Time 3:Julian 4:CarRot 5:ObsFlux 6:AdjFlux
        // The script uses $5 which is ObsFlux.

        // check prefix
        if (strncmp(line, target_prefix, 6) == 0) {
            char* p = line;
            // skip 4 tokens
            for (int k = 0; k < 4; k++) {
                while (*p && !isspace(*p)) p++;
                while (*p && isspace(*p)) p++;
            }
            if (*p) {
                float flux = atof(p);
                if (flux > 0) {
                    sum_flux += flux;
                    count++;
                }
            }
        }
    }
    fclose(fp);
    unlink(tmp_fn);

    // 5. Append if valid
    if (count > 0) {
        float avg = sum_flux / count;
        fp = fopenOurs(cache_fn, "a");
        if (fp) {
            fprintf(fp, "%.2f %.2f\n", target_fy, avg);
            fclose(fp);
            Serial.printf("SFHist: Appended %.2f %.2f (n=%d)\n", target_fy, avg, count);
        }
    } else {
        Serial.printf("SFHist: No data found for %s\n", target_prefix);
    }

    return true;
}

/* Retrieve the history file cache path, ensuring it is up to date.
 * Returns absolute path buffer valid until next call.
 */
const char* retrieveSolarFluxHistoryFile(void) {
    updateSolarFluxHistory();
    // return full path to our local file
    static char path[1024];
    snprintf(path, sizeof(path), "%s/solarflux-history.txt", our_dir.c_str());
    return path;
}

/* Update the local ssn-history.txt cache with new monthly averages.
 * If cache is missing, seed it from the backend server first.
 * Then check sidc.be for new data.
 */
static bool updateSSNHistory(void) {
    const char* cache_fn = "ssn-history.txt";
    static time_t last_check = 0;

    // only check once a day
    if (myNow() - last_check < 86400) return true;
    last_check = myNow();

    // 1. Seed if missing (Robust logic)
    FILE* fp = fopenOurs(cache_fn, "r");
    if (!fp) {
        const char* bundles[] = {"data", "/usr/local/share/hamclock"};
        for (auto* b : bundles) {
            char static_seed[1024];
            snprintf(static_seed, sizeof(static_seed), "%s/%s", b, cache_fn);
            FILE* seed = fopen(static_seed, "r");
            if (seed) {
                Serial.printf("SSNHist: Seeding cache from %s...\n", static_seed);
                fp = fopenOurs(cache_fn, "w");
                if (fp) {
                    char buf[1024];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), seed)) > 0) {
                        fwrite(buf, 1, n, fp);
                    }
                    fclose(fp);
                }
                fclose(seed);
                if (fp) break;
            }
        }

        // re-open read-only to verify
        fp = fopenOurs(cache_fn, "r");
        if (!fp) return false;
    }

    // 2. Scan for last entry date in cache to see if we need update
    // Format: YYYY.frac SSN
    float last_frac_year = 0;
    char line[100];
    while (fgets(line, sizeof(line), fp)) {
        float fy, val;
        if (sscanf(line, "%f %f", &fy, &val) == 2) last_frac_year = fy;
    }
    fclose(fp);

    // 3. Fetch live data
    // SILSO CSV: year;month;frac;ssn;...
    const char* src_url = "https://www.sidc.be/silso/DATA/SN_m_tot_V2.0.csv";
    const char* tmp_fn = "/tmp/ssn_update.csv";

    Serial.printf("SSNHist: Fetching update from %s\n", src_url);
    if (!curlDownload(src_url, tmp_fn)) return false;

    fp = fopen(tmp_fn, "r");
    if (!fp) return false;

    // 4. Parse and Append
    // We want to append lines that are NEWER than last_frac_year.
    // Ideally we append in order. The CSV is ordered.

    int added = 0;
    FILE* fout = NULL;  // open only if needed

    while (fgets(line, sizeof(line), fp)) {
        // year;month;frac;ssn;...
        // 1818;01;1818.042;...
        int y, m;
        float frac, ssn;
        // sscanf might be tricky with ; using standard format string.
        // Use manual tokenizing or replace ; with space first?
        // Let's replace ; with space
        for (char* p = line; *p; p++)
            if (*p == ';') *p = ' ';

        // now parse
        // year month decimal ssn ...
        // 1749 1 1749.042 96.7 ...
        if (sscanf(line, "%d %d %f %f", &y, &m, &frac, &ssn) == 4) {
            // Filter: 1,3,5,7,9,11
            if (m % 2 != 1) continue;

            // Recompute our fractional year: Year + (Month-1)/12.0
            float my_frac = y + (m - 1) / 12.0f;

            if (my_frac > last_frac_year + 0.001) {
                // New!
                if (!fout) fout = fopenOurs(cache_fn, "a");
                if (fout) {
                    fprintf(fout, "%.2f %.1f\n", my_frac, ssn);
                    added++;
                    last_frac_year = my_frac;  // advance
                }
            }
        }
    }

    fclose(fp);
    if (fout) fclose(fout);
    unlink(tmp_fn);

    if (added) Serial.printf("SSNHist: Appended %d new points\n", added);

    return true;
}

/* Retrieve the history file cache path, ensuring it is up to date.
 * Returns absolute path buffer valid until next call.
 */
const char* retrieveSSNHistoryFile(void) {
    updateSSNHistory();
    // return full path to our local file
    static char path[1024];
    snprintf(path, sizeof(path), "%s/ssn-history.txt", our_dir.c_str());
    return path;
}

/* return whether fresh SPCWX_FLUX data are ready, even if bad.
 */
static bool checkForNewSolarFlux(void) {
    if (myNow() < sf_cache.next_update) return (false);

    SolarFluxData sf;
    return (retrieveSolarFlux(sf));
}

/* retrieve DRAP and SPCWX_DRAP if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */

static const char drap_history_fn[] = "drap_history.txt";

static void saveDRAPHistory(const DRAPData& d) {
    FILE* fp = fopenOurs(drap_history_fn, "w");
    if (fp) {
        time_t now = myNow();
        for (int i = 0; i < DRAPDATA_NPTS; i++) {
            if (d.y[i] > 0) {
                // x[i] is age in hours (negative). t = now + x*3600
                time_t t = now + (time_t)(d.x[i] * 3600.0F);
                fprintf(fp, "%ld %.2f\n", (long)t, d.y[i]);
            }
        }
        fclose(fp);
    }
}

static void loadDRAPHistory(DRAPData& d) {
    FILE* fp = fopenOurs(drap_history_fn, "r");
    if (fp) {
        // init data
        memset(d.x, 0, DRAPDATA_NPTS * sizeof(float));
        memset(d.y, 0, DRAPDATA_NPTS * sizeof(float));

        time_t now = myNow();
        char line[50];
        while (fgets(line, sizeof(line), fp)) {
            long t;
            float v;
            if (sscanf(line, "%ld %f", &t, &v) == 2) {
                float age_sec = now - t;
                // map to bucket
                if (age_sec >= 0 && age_sec < DRAPDATA_PERIOD) {
                    int xi = DRAPDATA_NPTS * (DRAPDATA_PERIOD - age_sec) / DRAPDATA_PERIOD;
                    if (xi >= 0 && xi < DRAPDATA_NPTS) {
                        d.x[xi] = -age_sec / 3600.0F;
                        d.y[xi] = v;
                    }
                }
            }
        }
        fclose(fp);
        d.data_ok = true;  // assume if file exists we have something
    }
}

/* retrieve DRAP and SPCWX_DRAP if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */
bool retrieveDRAP(DRAPData& drap) {
    static bool history_loaded = false;
    if (!history_loaded) {
        loadDRAPHistory(drap_cache);
        history_loaded = true;
    }

    // check cache first
    if (myNow() < drap_cache.next_update) {
        drap = drap_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/text/drap_global_frequencies.txt";
    const char* tmp_fn = "/tmp/hc_drap.txt";

    Serial.printf("DRAP: Downloading %s\n", url);
    updateClocks(false);

    if (!curlDownload(url, tmp_fn)) {
        Serial.printf("DRAP: Download failed\n");
        drap_cache.next_update = myNow() + 300;
        return false;
    }

    FILE* fp = fopen(tmp_fn, "r");
    if (!fp) {
        Serial.printf("DRAP: Open %s failed\n", tmp_fn);
        drap_cache.next_update = myNow() + 300;
        return false;
    }

    // mark data as bad until proven otherwise
    space_wx[SPCWX_DRAP].value_ok = false;
    drap.data_ok = drap_cache.data_ok = false;

    float max_freq = 0;
    bool found_any = false;
    char line[1000];  // long lines

    while (fgets(line, sizeof(line), fp)) {
        // Skip comments and headers
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // Data lines might look like "  89 |  0.0  0.0 ..."
        // We look for the pipe symbol
        char* pipe = strchr(line, '|');
        if (pipe) {
            // All numbers after pipe are frequencies
            char* p = pipe + 1;
            char* next_p;
            while (*p) {
                // simple strtof with pointer advancement
                float val = strtof(p, &next_p);
                if (p == next_p) {
                    // No valid conversion, skip spaces or break
                    if (isspace(*p)) {
                        p++;
                        continue;
                    } else {
                        break;
                    }
                }

                if (val > max_freq) max_freq = val;
                found_any = true;
                p = next_p;
            }
        }
    }

    fclose(fp);
    unlink(tmp_fn);

    bool ok = false;
    if (found_any) {
        // Shift history left
        for (int i = 0; i < DRAPDATA_NPTS - 1; i++) {
            drap_cache.y[i] = drap_cache.y[i + 1];
            // x[i] will be regenerated
        }

        // Add new point at end
        drap_cache.y[DRAPDATA_NPTS - 1] = max_freq;

        // Regenerate X axis (ages)
        // 0 is oldest (-24h), NPTS-1 is newest (0h)
        for (int i = 0; i < DRAPDATA_NPTS; i++) {
            // age in hours. i=0 -> -24. i=NPTS-1 -> 0
            // linear lerp
            float age_hrs = -24.0F + (24.0F * i) / (float)(DRAPDATA_NPTS - 1);
            drap_cache.x[i] = age_hrs;
        }

        // Save
        saveDRAPHistory(drap_cache);

        // Update global stat
        space_wx[SPCWX_DRAP].value = max_freq;
        space_wx[SPCWX_DRAP].value_ok = true;
        drap.data_ok = drap_cache.data_ok = true;

        ok = true;
        Serial.printf("DRAP: Updated Max %.2f MHz\n", max_freq);
    } else {
        Serial.printf("DRAP: Parsing failed or no data\n");
    }

    drap_cache.next_update = ok ? nextRetrieval(PLOT_CH_DRAP, DRAPDATA_INTERVAL) : nextWiFiRetry(PLOT_CH_DRAP);
    drap = drap_cache;
    return ok;
}

/* return whether fresh SPCWX_DRAP data are ready, even if bad.
 */
bool checkForNewDRAP() {
    if (myNow() < drap_cache.next_update) return (false);

    DRAPData drap;
    return (retrieveDRAP(drap));
}

/* retrieve Kp and SPCWX_KP if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */
bool retrieveKp(KpData& kp) {
    // check cache first
    if (myNow() < kp_cache.next_update) {
        kp = kp_cache;
        return (true);
    }

    const char* url =
        "https://services.swpc.noaa.gov/products/"
        "noaa-planetary-k-index-forecast.json";
    const char* tmp_fn = "/tmp/hc_kp.json";
    bool ok = false;

    // reset status
    space_wx[SPCWX_KP].value_ok = false;
    kp.data_ok = kp_cache.data_ok = false;

    Serial.printf("Kp: %s\n", url);

    if (curlDownload(url, tmp_fn)) {
        updateClocks(false);
        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char* buf = (char*)malloc(fsize + 1);
            if (buf) {
                fread(buf, 1, fsize, fp);
                buf[fsize] = 0;

                StaticJsonDocument<20000> doc;
                DeserializationError err = deserializeJson(doc, buf);
                if (!err && doc.is<JsonArray>()) {
                    JsonArray arr = doc.as<JsonArray>();
                    int n_rows = arr.size();

                    const int now_i = KP_NHD * KP_VPD - 1;

                    int k = 0;
                    // Skip header (i=1)
                    for (int i = 1; i < n_rows && k < KP_NV; i++) {
                        JsonArray row = arr[i];
                        kp_cache.x[k] = (k - now_i) / (float)KP_VPD;
                        kp_cache.p[k] = row[1].as<float>();
                        k++;
                    }

                    if (k >= KP_NV / 2) {
                        // Current value is roughly at now_i if file alignment holds (7 days
                        // history) Safety check index
                        int val_i = now_i;
                        if (val_i >= k) val_i = k - 1;

                        space_wx[SPCWX_KP].value = kp_cache.p[val_i];
                        space_wx[SPCWX_KP].value_ok = true;
                        kp_cache.data_ok = true;
                        kp = kp_cache;
                        ok = true;
                        Serial.printf("Kp: Now Val %.2f\n", kp.p[val_i]);
                    }
                } else {
                    Serial.printf("Kp: JSON Err %s\n", err.c_str());
                }
                free(buf);
            }
            fclose(fp);
        }
    } else {
        Serial.printf("Kp: Download failed\n");
    }

    kp_cache.next_update = ok ? nextRetrieval(PLOT_CH_KP, KP_INTERVAL) : nextWiFiRetry(PLOT_CH_KP);
    return ok;
}

/* return whether fresh SPCWX_KP data are ready, even if bad.
 */
static bool checkForNewKp(void) {
    if (myNow() < kp_cache.next_update) return (false);

    KpData kp;
    return (retrieveKp(kp));
}

/* retrieve DST and SPCWX_DST if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 * Uses NOAA SWPC JSON (HTTPS via libcurl)
 */
bool retrieveDST(DSTData& dst) {
    // check cache first
    if (myNow() < dst_cache.next_update) {
        dst = dst_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/products/kyoto-dst.json";
    const char* tmp_fn = "/tmp/hc_dst.json";
    bool ok = false;

    // mark value as bad until proven otherwise
    space_wx[SPCWX_DST].value_ok = false;
    dst.data_ok = dst_cache.data_ok = false;

    Serial.printf("DST: %s\n", url);

    if (curlDownload(url, tmp_fn)) {
        updateClocks(false);
        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char* buf = (char*)malloc(fsize + 1);
            if (buf) {
                fread(buf, 1, fsize, fp);
                buf[fsize] = 0;

                StaticJsonDocument<20000> doc;
                DeserializationError err = deserializeJson(doc, buf);
                if (!err && doc.is<JsonArray>()) {
                    JsonArray arr = doc.as<JsonArray>();

                    // Data is [ [Time, DST], ... ] sorted by time.
                    // We need last DST_NV values.
                    // Format "2024-03-01 00:00:00"

                    int count = arr.size();
                    time_t now = myNow();
                    int dst_i = 0;

                    // Fill from end (newest)
                    // Skip header (0)
                    for (int i = count - 1; i > 0 && dst_i < DST_NV; i--) {
                        JsonArray row = arr[i];
                        const char* t_str = row[0];
                        int val = row[1].as<int>();

                        // Format: YYYY-MM-DD HH:MM:SS
                        // crackISO8601 expects ISO T separator, but this has space.
                        // parse_noaa_json_time handles both.
                        time_t t_stamp = parse_noaa_json_time(t_str);

                        float age_hrs = (t_stamp - now) / 3600.0F;

                        // Cache expects oldest first at index 0..DST_NV-1
                        // But we are reading newest first.
                        // Let's store in a temp buffer then reverse-copy or fill smartly.
                        // Actually, we can just fill from end of cache.
                        int cache_idx = DST_NV - 1 - dst_i;
                        dst_cache.age_hrs[cache_idx] = age_hrs;
                        dst_cache.values[cache_idx] = (float)val;

                        dst_i++;
                    }

                    if (dst_i > 0) {
                        // If we didn't fill the whole array, shift valid data to end?
                        // Original code didn't seem to handle partial fills well for
                        // alignment, but let's assume we got enough.
                        if (dst_i < DST_NV) {
                            Serial.printf("DST: Short data, expected %d got %d\n", DST_NV, dst_i);
                            // Shift to end
                            int shift = DST_NV - dst_i;
                            for (int j = 0; j < dst_i; j++) {
                                dst_cache.age_hrs[j] = dst_cache.age_hrs[j + shift];
                                dst_cache.values[j] = dst_cache.values[j + shift];
                            }
                        }

                        // Current value is the latest (last in array)
                        space_wx[SPCWX_DST].value = dst_cache.values[DST_NV - 1];
                        space_wx[SPCWX_DST].value_ok = true;
                        dst_cache.data_ok = true;
                        dst = dst_cache;
                        ok = true;
                        Serial.printf("DST: Last %g\n", space_wx[SPCWX_DST].value);
                    }
                }
                free(buf);
            }
            fclose(fp);
        }
    } else {
        Serial.printf("DST: Download failed\n");
    }

    // set next update
    dst_cache.next_update = ok ? nextRetrieval(PLOT_CH_DST, DST_INTERVAL) : nextWiFiRetry(PLOT_CH_DST);
    return (ok);
}

/* return whether fresh SPCWX_DST data are ready, even if bad.
 */
static bool checkForNewDST(void) {
    if (myNow() < dst_cache.next_update) return (false);

    DSTData dst;
    return (retrieveDST(dst));
}

/* retrieve XRay and SPCWX_XRAY if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */
/* retrieve XRay and SPCWX_XRAY if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 * Uses NOAA SWPC JSON via wget
 */
bool retrieveXRay(XRayData& xray) {
    // check cache first
    if (myNow() < xray_cache.next_update) {
        xray = xray_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/json/goes/primary/xrays-1-day.json";
    const char* tmp_fn = "/tmp/hc_xray.json";
    bool ok = false;

    // mark value as bad until proven otherwise
    space_wx[SPCWX_XRAY].value_ok = false;
    xray.data_ok = xray_cache.data_ok = false;

    Serial.printf("XRay: %s\n", url);
    if (curlDownload(url, tmp_fn)) {
        updateClocks(false);
        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char* buf = (char*)malloc(fsize + 1);
            if (buf) {
                fread(buf, 1, fsize, fp);
                buf[fsize] = 0;

                // 1 day JSON is ~650KB.
                // Using DynamicJsonDocument (heap)
                DynamicJsonDocument doc(1024 * 1024);
                DeserializationError err = deserializeJson(doc, buf);

                if (!err && doc.is<JsonArray>()) {
                    JsonArray arr = doc.as<JsonArray>();
                    time_t t0 = myNow();

                    // Reset cache
                    for (int i = 0; i < XRAY_NV; i++) {
                        xray_cache.l[i] = -9.0;  // log scale small
                        xray_cache.s[i] = -9.0;
                        xray_cache.x[i] = (i - XRAY_NV) / 6.0;  // hours ago
                    }

                    float final_l = 0;
                    bool found_any = false;

                    for (JsonObject row : arr) {
                        const char* tag = row["time_tag"];
                        time_t t = parse_noaa_json_time(tag);
                        if (t == 0) continue;

                        // Map time to index.
                        // x = (t - t0)/3600.0.
                        // index = x*6 + XRAY_NV.
                        // Or reverse:
                        // t - t0 (seconds ago, negative)
                        // bin = (t - t0)/600 + XRAY_NV. (10 mins = 600s)

                        int idx = (int)((t - t0) / 600 + XRAY_NV);
                        if (idx >= 0 && idx < XRAY_NV) {
                            const char* energy = row["energy"];
                            float flux = row["flux"];
                            if (flux <= 0) flux = 1e-9;

                            if (strstr(energy, "0.05-0.4")) {
                                xray_cache.s[idx] = log10f(flux);
                            } else {
                                xray_cache.l[idx] = log10f(flux);
                                // Is this the absolute latest?
                                // JSON is usually sorted.
                                // We want the value at XRAY_NV-1 (Now).
                                // But mapping might put it at XRAY_NV-1 or -2.
                                // Just track the latest valid time seen?
                                final_l = flux;
                                found_any = true;
                            }
                        }
                    }

                    // Fill gaps? (Nearest neighbor or hold)
                    // Simple hole filling
                    for (int i = 1; i < XRAY_NV; i++) {
                        if (xray_cache.l[i] < -8.0 && xray_cache.l[i - 1] > -8.0) xray_cache.l[i] = xray_cache.l[i - 1];
                        if (xray_cache.s[i] < -8.0 && xray_cache.s[i - 1] > -8.0) xray_cache.s[i] = xray_cache.s[i - 1];
                    }

                    if (found_any) {
                        space_wx[SPCWX_XRAY].value = final_l;
                        space_wx[SPCWX_XRAY].value_ok = true;
                        xray_cache.data_ok = true;
                        xray = xray_cache;
                        ok = true;
                        Serial.printf("XRay: Last %.2e\n", final_l);
                    }
                }
                free(buf);
            }
            fclose(fp);
        }
    } else {
        Serial.printf("XRay: Download failed\n");
    }

    xray_cache.next_update = ok ? nextRetrieval(PLOT_CH_XRAY, XRAY_INTERVAL) : nextWiFiRetry(PLOT_CH_XRAY);
    return (ok);
}

/* return whether fresh SPCWX_XRAY data are ready, even if bad.
 */
static bool checkForNewXRay(void) {
    if (myNow() < xray_cache.next_update) return (false);

    XRayData xray;
    return (retrieveXRay(xray));
}

/* retrieve BzBt data and SPCWX_BZBT if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */
bool retrieveBzBt(BzBtData& bzbt) {
    // check cache first
    if (myNow() < bzbt_cache.next_update) {
        bzbt = bzbt_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/products/solar-wind/mag-1-day.json";
    const char* tmp_fn = "/tmp/hc_bzbt.json";
    bool ok = false;
    time_t t0 = myNow();

    // mark data as bad until proven otherwise
    space_wx[SPCWX_BZ].value_ok = false;
    bzbt.data_ok = bzbt_cache.data_ok = false;

    Serial.printf("BzBt: %s\n", url);
    if (curlDownload(url, tmp_fn)) {
        updateClocks(false);
        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char* buf = (char*)malloc(fsize + 1);
            if (buf) {
                fread(buf, 1, fsize, fp);
                buf[fsize] = 0;

                // 100KB+ for 1-day data, use 1MB buffer for safety
                DynamicJsonDocument doc(1024 * 1024);
                DeserializationError err = deserializeJson(doc, buf);
                if (!err && doc.is<JsonArray>()) {
                    JsonArray arr = doc.as<JsonArray>();
                    int n_rows = arr.size();

                    int idx = 0;
                    // Dynamically calculate step to fit 24h into BZBT_NV (150)
                    // n_rows includes header. data is n_rows-1.
                    int n_data = n_rows - 1;
                    int step = n_data / BZBT_NV;
                    if (step < 1) step = 1;

                    for (int i = 1; i < n_rows && idx < BZBT_NV; i += step) {
                        JsonArray row = arr[i];
                        time_t unixs = parse_noaa_json_time(row[0]);
                        if (unixs == 0) continue;

                        // Cols: time, bx, by, bz, lon, lat, bt
                        bzbt_cache.bz[idx] = row[3].as<float>();
                        bzbt_cache.bt[idx] = row[6].as<float>();

                        bzbt_cache.x[idx] = unixs < t0 ? (unixs - t0) / 3600.0F : 0;
                        idx++;
                    }

                    if (idx >= BZBT_NV / 2) {
                        space_wx[SPCWX_BZ].value = bzbt_cache.bz[idx - 1];
                        space_wx[SPCWX_BZ].value_ok = true;
                        bzbt_cache.data_ok = true;
                        bzbt = bzbt_cache;
                        ok = true;
                        Serial.printf("BzBt: Last %.1f (count %d)\n", space_wx[SPCWX_BZ].value, idx);
                    } else {
                        Serial.printf("BzBt: Too few points %d (step %d, n_rows %d)\n", idx, step, n_rows);
                    }
                } else {
                    Serial.printf("BzBt: JSON Parse Error: %s\n", err.c_str());
                }
                free(buf);
            }
            fclose(fp);
        }
    } else {
        Serial.printf("BzBt: Download failed\n");
    }

    bzbt_cache.next_update = ok ? nextRetrieval(PLOT_CH_BZBT, BZBT_INTERVAL) : nextWiFiRetry(PLOT_CH_BZBT);
    return (ok);
}

/* return whether fresh SPCWX_BZBT data are ready, even if bad.
 */
static bool checkForNewBzBt(void) {
    if (myNow() < bzbt_cache.next_update) return (false);
    BzBtData bzbt;
    return (retrieveBzBt(bzbt));
}

/* retrieve solar wind and SPCWX_SOLWIND if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */
/* parse NOAA JSON time tag "2026-01-31 16:35:00.000"
 */

/* retrieve solar wind and SPCWX_SOLWIND if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 * Uses NOAA SWPC JSON (HTTPS via wget)
 */
bool retrieveSolarWind(SolarWindData& sw) {
    // check cache first
    if (myNow() < sw_cache.next_update) {
        sw = sw_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/products/solar-wind/plasma-1-day.json";
    const char* tmp_fn = "/tmp/hc_swind.json";
    bool ok = false;

    // mark value as bad until proven otherwise
    space_wx[SPCWX_SOLWIND].value_ok = false;
    sw.data_ok = sw_cache.data_ok = false;

    Serial.printf("SolWind: %s\n", url);
    if (curlDownload(url, tmp_fn)) {
        updateClocks(false);
        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char* buf = (char*)malloc(fsize + 1);
            if (buf) {
                fread(buf, 1, fsize, fp);
                buf[fsize] = 0;

                // ~70KB for 1-day data, use 1MB buffer for safety
                DynamicJsonDocument doc(1024 * 1024);
                DeserializationError err = deserializeJson(doc, buf);
                if (!err && doc.is<JsonArray>()) {
                    JsonArray arr = doc.as<JsonArray>();
                    int n_rows = arr.size();

                    time_t t0 = myNow();
                    time_t start_t = t0 - SWIND_PER;
                    time_t prev_unixs = 0;
                    float max_y = 0;

                    sw_cache.n_values = 0;

                    // Iterate (skip header)
                    for (int i = 1; i < n_rows && sw_cache.n_values < SWIND_MAXN; i++) {
                        JsonArray row = arr[i];
                        time_t unixs = parse_noaa_json_time(row[0]);
                        if (unixs == 0) continue;

                        float density = row[1].as<float>();
                        float speed = row[2].as<float>();
                        float this_y = density * speed * 1e-3;

                        if (this_y > max_y) max_y = this_y;

                        // Check interval (same logic as before)
                        if ((unixs < start_t || unixs - prev_unixs < SWIND_DT) && sw_cache.n_values != SWIND_MAXN - 1) {
                            // accumulate max_y, check next point
                            continue;
                        }
                        prev_unixs = unixs;

                        sw_cache.x[sw_cache.n_values] = (t0 - unixs) / (-3600.0F);  // hours back (neg)
                        sw_cache.y[sw_cache.n_values] = max_y;

                        max_y = 0;  // reset for next bin
                        sw_cache.n_values++;
                    }

                    if (sw_cache.n_values >= SWIND_MINN) {
                        space_wx[SPCWX_SOLWIND].value = sw_cache.y[sw_cache.n_values - 1];
                        space_wx[SPCWX_SOLWIND].value_ok = true;
                        sw_cache.data_ok = true;
                        sw = sw_cache;
                        ok = true;
                        Serial.printf("SolWind: Last %.2f (n=%d)\n", space_wx[SPCWX_SOLWIND].value, sw_cache.n_values);
                    }
                }
                free(buf);
            }
            fclose(fp);
        }
    } else {
        Serial.printf("SolWind: Download failed\n");
    }

    sw_cache.next_update = ok ? nextRetrieval(PLOT_CH_SOLWIND, SWIND_INTERVAL) : nextWiFiRetry(PLOT_CH_SOLWIND);
    return (ok);
}

/* return whether fresh SPCWX_SOLWIND data are ready, even if bad.
 */
static bool checkForNewSolarWind(void) {
    if (myNow() < sw_cache.next_update) return (false);

    SolarWindData sw;
    return (retrieveSolarWind(sw));
}

/* retrieve NOAA space weather indices and SPCWX_NOAASPW if it's time, else use
 * cache. return whether transaction was ok (even if data was not)
 */
/* retrieve NOAA space weather indices and SPCWX_NOAASPW if it's time, else use
 * cache. return whether transaction was ok (even if data was not)
 * Uses NOAA SWPC JSON (HTTPS via libcurl)
 */
bool retrieveNOAASWx(NOAASpaceWxData& noaasw) {
    // check cache first
    if (myNow() < noaasw_cache.next_update) {
        noaasw = noaasw_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/products/noaa-scales.json";
    const char* tmp_fn = "/tmp/hc_noaasw.json";
    bool ok = false;

    // mark data as bad until proven otherwise
    space_wx[SPCWX_NOAASPW].value_ok = false;
    noaasw.data_ok = noaasw_cache.data_ok = false;

    Serial.printf("NOAASW: %s\n", url);

    if (curlDownload(url, tmp_fn)) {
        updateClocks(false);

        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            char* buf = (char*)malloc(fsize + 1);
            if (buf) {
                fread(buf, 1, fsize, fp);
                buf[fsize] = 0;

                StaticJsonDocument<4096> doc;
                DeserializationError err = deserializeJson(doc, buf);
                if (!err && doc.is<JsonObject>()) {
                    JsonObject root = doc.as<JsonObject>();

                    // JSON keys are "0" (today), "1" (tomorrow), "2" (next day) ...
                    // Our columns are Now, D+1, D+2 ...
                    // N_NOAASW_V should be 3 or 4.

                    ok = true;
                    int noaasw_max = 0;

                    // Map JSON keys to our columns 0..N-1
                    // JSON has "0", "1", "2"...

                    for (int j = 0; j < N_NOAASW_V; j++) {
                        char key[2];
                        snprintf(key, sizeof(key), "%d", j);

                        if (root.containsKey(key)) {
                            JsonObject day = root[key];
                            // R, S, G

                            // R is row 0
                            if (day.containsKey("R")) {
                                int val = day["R"]["Scale"].as<int>();
                                noaasw_cache.val[0][j] = val;
                                if (val > noaasw_max) noaasw_max = val;
                            }

                            // S is row 1
                            if (day.containsKey("S")) {
                                int val = day["S"]["Scale"].as<int>();
                                noaasw_cache.val[1][j] = val;
                                if (val > noaasw_max) noaasw_max = val;
                            }

                            // G is row 2
                            if (day.containsKey("G")) {
                                int val = day["G"]["Scale"].as<int>();
                                noaasw_cache.val[2][j] = val;
                                if (val > noaasw_max) noaasw_max = val;
                            }

                        } else {
                            // Zero fill if missing
                            for (int i = 0; i < N_NOAASW_C; i++) noaasw_cache.val[i][j] = 0;
                        }
                    }

                    // R, S, G chars are already in noaasw_cache.cat from initializer

                    noaasw_cache.data_ok = true;
                    space_wx[SPCWX_NOAASPW].value = (float)noaasw_max;
                    space_wx[SPCWX_NOAASPW].value_ok = true;
                    noaasw = noaasw_cache;

                    Serial.printf("NOAASW: max %d\n", noaasw_max);
                } else {
                    Serial.print("NOAASW: JSON parse failed\n");
                }
                free(buf);
            }
            fclose(fp);
        }
    } else {
        Serial.print("NOAASW: Download failed\n");
    }

    // set next update
    noaasw_cache.next_update = ok ? nextRetrieval(PLOT_CH_NOAASPW, NOAASPW_INTERVAL) : nextWiFiRetry(PLOT_CH_NOAASPW);

    return (ok);
}

/* return whether fresh SPCWX_NOAASPW data are ready, even if bad.
 */
static bool checkForNewNOAASWx(void) {
    if (myNow() < noaasw_cache.next_update) return (false);
    NOAASpaceWxData noaasw;
    return (retrieveNOAASWx(noaasw));
}

/* retrieve aurora and SPCWX_AURORA if it's time, else use cache.
 * return whether transaction was ok (even if data was not)
 */

static const char aurora_history_fn[] = "aurora_history.txt";

static void saveAuroraHistory(const AuroraData& a) {
    FILE* fp = fopenOurs(aurora_history_fn, "w");
    if (fp) {
        time_t now = myNow();
        for (int i = 0; i < a.n_points; i++) {
            // save as unix time and percent
            time_t t = now + (time_t)(a.age_hrs[i] * 3600.0F);
            fprintf(fp, "%ld %.1f\n", (long)t, a.percent[i]);
        }
        fclose(fp);
    }
}

static void loadAuroraHistory(AuroraData& a) {
    FILE* fp = fopenOurs(aurora_history_fn, "r");
    if (fp) {
        a.n_points = 0;
        time_t now = myNow();
        char line[50];
        while (a.n_points < AURORA_MAXPTS && fgets(line, sizeof(line), fp)) {
            long t;
            float p;
            if (sscanf(line, "%ld %f", &t, &p) == 2) {
                float age = (t - now) / 3600.0F;
                // keep if not too old or in future
                // N.B. AURORA_MAXAGE is max plot age, but we can store a bit more if we
                // want, but let's stick to what fits the array and is reasonably valid.
                if (age > -100.0F && age <= 0) {
                    a.age_hrs[a.n_points] = age;
                    a.percent[a.n_points] = p;
                    a.n_points++;
                }
            }
        }
        fclose(fp);

        // if we loaded anything, mark as ok so we can plot immediately
        if (a.n_points > 0) {
            a.data_ok = true;
            // set next update to now so we fetch fresh data immediately too
            a.next_update = 0;
        }
    }
}
bool retrieveAurora(AuroraData& aurora) {
    // load history once
    static bool history_loaded;
    if (!history_loaded) {
        loadAuroraHistory(aurora_cache);
        history_loaded = true;
    }

    // check cache first
    if (myNow() < aurora_cache.next_update) {
        aurora = aurora_cache;
        return (true);
    }

    const char* url = "https://services.swpc.noaa.gov/json/ovation_aurora_latest.json";
    const char* tmp_fn = "/tmp/hc_aurora.json";

    Serial.printf("AURORA: Downloading %s\n", url);
    updateClocks(false);

    if (!curlDownload(url, tmp_fn)) {
        Serial.printf("AURORA: Download failed\n");
        aurora_cache.next_update = myNow() + 300;
        return false;
    }

    FILE* fp = fopen(tmp_fn, "r");
    if (!fp) {
        Serial.printf("AURORA: Open %s failed\n", tmp_fn);
        aurora_cache.next_update = myNow() + 300;
        return false;
    }

    // mark data as bad until proven otherwise
    space_wx[SPCWX_AURORA].value_ok = false;
    aurora.data_ok = aurora_cache.data_ok = false;

    // init state
    float max_percent = 0;
    bool found_any = false;

    // Manual parse of huge JSON grid [[lon,lat,val],...]
    // We look for patterns of [n,n,n] and take the 3rd n.
    // This avoids loading the entire 1MB+ structure into ArduinoJson memory.

    char buf[256];
    // Simple state machine or just scan?
    // The format is "coordinates": [[0, -90, 3], [0, -89, 0], ...]

    // Fast forward to "coordinates"
    bool in_coords = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (!in_coords) {
            if (strstr(buf, "\"coordinates\"")) in_coords = true;
        }

        if (in_coords) {
            // We are in the data section. Scan for bracketed triplets.
            // Example: [0, -90, 3],
            // We can just iterate char by char or use ptrs
            char* p = buf;
            while (*p) {
                // Look for start of array [
                if (*p == '[') {
                    int lon, lat, val;
                    // Try to match 3 ints
                    // Note: sscanf need enough chars, but buf might cut off.
                    // However, newlines usually split nicely in JSON pretty print,
                    // but if it's minified one line, we need to be careful.
                    // The NOAA file we saw was one huge line? Or pretty?
                    // The snippet showed one line.
                    // So fgets might read chunks.

                    // We rely on sscanf to parse current position.
                    // If it matches, we advance.
                    if (sscanf(p, "[%d, %d, %d]", &lon, &lat, &val) == 3) {
                        // We found a point
                        if (val > max_percent) max_percent = (float)val;
                        found_any = true;
                    }
                    // Note: valid lon/lat can be neg. sscanf handles that.
                }
                p++;
            }
        }
    }

    fclose(fp);
    unlink(tmp_fn);

    bool ok = false;
    if (found_any) {
        // We have a single 'current' value.
        // Append to history.

        // Shift history to make room at the end
        if (aurora_cache.n_points < AURORA_MAXPTS) {
            aurora_cache.n_points++;
        }

        // Shift older data to lower indices (0 is oldest)
        // Wait, other functions: element 0 is oldest?
        // plotXY uses x[] and y[].
        // If x is -24, -23.5 ... 0.
        // Then 0 is newest.
        // So index N-1 is newest.

        for (int i = 0; i < aurora_cache.n_points - 1; i++) {
            // shift age simply by 1 position, we will recalibrate ages below
            aurora_cache.age_hrs[i] = aurora_cache.age_hrs[i + 1];
            aurora_cache.percent[i] = aurora_cache.percent[i + 1];
        }

        // Set newest
        int new_idx = aurora_cache.n_points - 1;
        aurora_cache.age_hrs[new_idx] = 0;  // provisional
        aurora_cache.percent[new_idx] = max_percent;

        // Enforce consistent timing for the graph.
        // This effectively snaps the history to the polling interval,
        // closing any gaps. This is acceptable for this simple monitor.
        for (int i = 0; i < aurora_cache.n_points; i++) {
            aurora_cache.age_hrs[i] = -(aurora_cache.n_points - 1 - i) * (AURORA_INTERVAL / 3600.0F);
        }

        // Save history
        saveAuroraHistory(aurora_cache);

        // Value
        space_wx[SPCWX_AURORA].value = max_percent;
        space_wx[SPCWX_AURORA].value_ok = true;
        aurora.data_ok = aurora_cache.data_ok = true;

        // Cache ready
        aurora = aurora_cache;
        ok = true;

        Serial.printf("AURORA: Updated Max %.0f%%\n", max_percent);
    } else {
        Serial.printf("AURORA: Parsing failed or no data found\n");
    }

    aurora_cache.next_update = ok ? nextRetrieval(PLOT_CH_AURORA, AURORA_INTERVAL) : nextWiFiRetry(PLOT_CH_AURORA);
    return ok;
}

/* return whether fresh SPCWX_AURORA data are ready, even if bad.
 */
bool checkForNewAurora() {
    if (myNow() < aurora_cache.next_update) return (false);
    AuroraData a;
    return (retrieveAurora(a));
}

/* update all space_wx stats but no faster than their respective panes would do.
 * return whether any actually updated.
 */
bool checkForNewSpaceWx() {
    // check each
    bool sf = checkForNewSolarFlux();
    bool kp = checkForNewKp();
    bool ds = checkForNewDST();
    bool xr = checkForNewXRay();
    bool bz = checkForNewBzBt();
    bool dr = checkForNewDRAP();
    bool sw = checkForNewSolarWind();
    bool ss = checkForNewSunSpots();
    bool na = checkForNewNOAASWx();
    bool au = checkForNewAurora();

    // check whether any changed
    bool any_new = sf || ds || kp || xr || bz || dr || sw || ss || na || au;

    // if so redo ranking unless Auto
    if (any_new && spcwx_chmask == SPCWX_AUTO) sortSpaceWx();

    return (any_new);
}

/* one-time setup
 */
void initSpaceWX(void) {
    // init all space_wx m and b
    bool mb_ok = initSWFit();
    if (!mb_ok) Serial.println("RANKSW: no ranking available -- using default");

    // init user selection ranking
    if (!NVReadUInt32(NV_SPCWXCHOICE, &spcwx_chmask)) {
        spcwx_chmask = SPCWX_AUTO;
        NVWriteUInt32(NV_SPCWXCHOICE, spcwx_chmask);
    }
    Serial.printf("SPCWX: initial choice mask 0x%08x\n", spcwx_chmask);

    // unless auto, set rank to match choice mask
    if (spcwx_chmask != SPCWX_AUTO) {
        // N.B. assign ranks in same order as runNCDXFSpcWxMenu()
        int rank = 0;
        for (int i = 0; i < SPCWX_N; i++)
            space_wx[i].rank = (spcwx_chmask & (1 << i)) ? rank++ : 99;  // 0 is highest rank
    }
}
