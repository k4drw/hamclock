/* manage PSKReporter, WSPR and RBN records and drawing.
 */

#include "HamClock.h"
#include "src/hal/linux/Serial.h"
#include "urlencode.h"

// global state for webserver
uint8_t psk_mask;          // one of PSKModeBits
uint32_t psk_bands;        // bitmask of HamBandSetting
uint16_t psk_maxage_mins;  // query period, minutes
uint8_t psk_showdist;      // show max distances, else count
uint8_t psk_showpath;      // show paths, else not

// query urls
static const char rbn_host[] = "telnet.reversebeacon.net";
static const int rbn_port = 7000;

// color config
#define LIVE_COLOR RGB565(80, 80, 255)  // title color

// private state
static DXSpot* reports;                 // malloced list of all reports, not just TST_PSKBAND
static int n_reports;                   // count of reports used in psk_bands, might be < n_malloced
static int n_malloced;                  // total n malloced in reports[]
static int spot_maxrpt[HAMBAND_N];      // indices into reports[] for the farthest
                                        // spot per band
static PSKBandStats bstats[HAMBAND_N];  // band stats

// RBN state
static WiFiClient rbn_client;
static bool rbn_login_sent;
static uint32_t rbn_last_activity;
static bool rbn_filter_sent;

// layout
#define SUBHEAD_DYUP 15                                                         // distance up from bottom to subheading
#define TBLHGAP (PLOTBOX123_W / 20)                                             // table horizontal gap
#define TBLCOLW (43 * PLOTBOX123_W / 100)                                       // table column width
#define TBLROWH ((PLOTBOX123_H - LISTING_Y0 - SUBHEAD_DYUP) / (HAMBAND_N / 2))  // table row height

// handy test and set whether a band is in use
#define SET_PSKBAND(b) (psk_bands |= (1 << (b)))                               // record that band b paths are displayed
#define TST_PSKBAND(b) ((b) != HAMBAND_NONE && (psk_bands & (1 << (b))) != 0)  // test if band b displayed

/* draw a distance target marker at Raw s with the given fill color.
 */
static void drawDistanceTarget(const SCoord& s, ColorSelection id) {
    // ignore if no dots
    if (getSpotLabelType() == LBL_NONE) return;

    // get radius
    uint16_t dot_r = getRawSpotRadius(id);

    // get colors
    uint16_t fill_color = getMapColor(id);
    uint16_t cross_color = getGoodTextColor(fill_color);

    // raw looks nicer

    tft.fillCircleRaw(s.x, s.y, dot_r, fill_color);
    tft.drawCircleRaw(s.x, s.y, dot_r, cross_color);
    tft.drawLineRaw(s.x - dot_r, s.y, s.x + dot_r, s.y, 1, cross_color);
    tft.drawLineRaw(s.x, s.y - dot_r, s.x, s.y + dot_r, 1, cross_color);
}

/* return whether the given age, in minutes, is allowed.
 */
bool maxPSKageOk(int m) { return (m == 15 || m == 30 || m == 60 || m == 360 || m == 1440); }

/* get NV settings related to PSK
 */
void initPSKState() {
    if (!NVReadUInt8(NV_PSK_MODEBITS, &psk_mask)) {
        // default PSK of grid
        psk_mask = PSKMB_PSK | PSKMB_OFDE;
        NVWriteUInt8(NV_PSK_MODEBITS, psk_mask);
    }
    if (!NVReadUInt32(NV_PSK_BANDS, &psk_bands)) {
        // default all ham_bands
        psk_bands = 0;
        for (int i = 0; i < HAMBAND_N; i++) SET_PSKBAND(i);
        NVWriteUInt32(NV_PSK_BANDS, psk_bands);
    }
    if (!NVReadUInt16(NV_PSK_MAXAGE, &psk_maxage_mins) || !maxPSKageOk(psk_maxage_mins)) {
        // default 30 minutes
        psk_maxage_mins = 30;
        NVWriteUInt16(NV_PSK_MAXAGE, psk_maxage_mins);
    }
    if (!NVReadUInt8(NV_PSK_SHOWDIST, &psk_showdist)) {
        psk_showdist = 0;
        NVWriteUInt8(NV_PSK_SHOWDIST, psk_showdist);
    }
    if (!NVReadUInt8(NV_PSK_SHOWPATH, &psk_showpath)) {
        psk_showpath = 1;  // default on
        NVWriteUInt8(NV_PSK_SHOWPATH, psk_showpath);
    }
}

/* save NV settings related to PSK
 */
void savePSKState() {
    NVWriteUInt8(NV_PSK_MODEBITS, psk_mask);
    NVWriteUInt32(NV_PSK_BANDS, psk_bands);
    NVWriteUInt16(NV_PSK_MAXAGE, psk_maxage_mins);
    NVWriteUInt8(NV_PSK_SHOWDIST, psk_showdist);
    NVWriteUInt8(NV_PSK_SHOWPATH, psk_showpath);
}

/* draw a target at the farthest spot in each active band as needed.
 */
void drawFarthestPSKSpots() {
    // proceed unless not wanted or not in use`
    if (getSpotLabelType() == LBL_NONE || findPaneForChoice(PLOT_CH_PSK) == PANE_NONE) return;

    // draw each that are enabled
    for (int i = 0; i < HAMBAND_N; i++) {
        PSKBandStats& pbs = bstats[i];
        if (pbs.maxkm > 0 && TST_PSKBAND(i)) {
            int tw = getRawSpotRadius(findColSel((HamBandSetting)i));
            SCoord s;
            ll2s(pbs.maxll, s, tw);
            if (overMap(s)) {
                ll2sRaw(pbs.maxll, s, tw);
                drawDistanceTarget(s, findColSel((HamBandSetting)i));
            }
        }
    }
}

/* draw the PSK pane in the given box
 */
static void drawPSKPane(const SBox& box) {
    // clear
    prepPlotBox(box);

    // handy
    bool use_call = (psk_mask & PSKMB_CALL) != 0;
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;
    bool ispsk = (psk_mask & PSKMB_SRCMASK) == PSKMB_PSK;
    bool iswspr = (psk_mask & PSKMB_SRCMASK) == PSKMB_WSPR;
    bool isrbn = (psk_mask & PSKMB_SRCMASK) == PSKMB_RBN;

    // title
    static const char* title = "Live Spots";
    selectFontStyle(LIGHT_FONT, SMALL_FONT);
    uint16_t tw = getTextWidth(title);
    tft.setTextColor(LIVE_COLOR);
    tft.setCursor(box.x + (box.w - tw) / 2, box.y + PANETITLE_H);
    tft.print(title);

    // set name to call or 4x grid
    char name[20];
    if (use_call) {
        strcpy(name, getCallsign());
    } else {
        char de_maid[MAID_CHARLEN];
        getNVMaidenhead(NV_DE_GRID, de_maid);
        snprintf(name, sizeof(name), "%.4s", de_maid);
    }

    // show how and when
    selectFontStyle(LIGHT_FONT, FAST_FONT);
    tft.setTextColor(RA8875_WHITE);
    char where_how[100];
    snprintf(where_how, sizeof(where_how), "%s %s - %s %d %s", of_de ? "of" : "by", name,
             (ispsk ? "PSK" : (iswspr ? "WSPR" : (isrbn ? "RBN" : "???"))),
             psk_maxage_mins < 60 ? psk_maxage_mins : psk_maxage_mins / 60,
             psk_maxage_mins < 60 ? "mins" : (psk_maxage_mins == 60 ? "hour" : "hrs"));
    uint16_t whw = getTextWidth(where_how);
    tft.setCursor(box.x + (box.w - whw) / 2, box.y + SUBTITLE_Y0);
    tft.print(where_how);

    // table
    for (int i = 0; i < HAMBAND_N; i++) {
        int row = i % (HAMBAND_N / 2);
        int col = i / (HAMBAND_N / 2);
        uint16_t x = box.x + TBLHGAP + col * (TBLCOLW + TBLHGAP);
        uint16_t y = box.y + LISTING_Y0 + row * TBLROWH;
        char report[30];
        if (psk_showdist) {
            float d = bstats[i].maxkm;
            if (!showDistKm()) d *= MI_PER_KM;
            snprintf(report, sizeof(report), "%3sm %5.0f", findBandName((HamBandSetting)i), d);
        } else
            snprintf(report, sizeof(report), "%3sm %5d", findBandName((HamBandSetting)i), bstats[i].count);
        if (TST_PSKBAND(i)) {
            uint16_t map_col = getMapColor(findColSel((HamBandSetting)i));
            uint16_t txt_col = getGoodTextColor(map_col);
            tft.fillRect(x, y - LISTING_OS + 1, TBLCOLW, TBLROWH - 3,
                         map_col);  // leave black below
            tft.setTextColor(txt_col);
            tft.setCursor(x + 2, y);
            tft.print(report);
        } else {
            // disabled, always show but diminished
            tft.fillRect(x, y - LISTING_OS + 1, TBLCOLW, TBLROWH - 3, RA8875_BLACK);
            tft.setTextColor(GRAY);
            tft.setCursor(x + 2, y);
            tft.print(report);
        }
    }

    // caption
    const char* label = psk_showdist ? (showDistKm() ? "Max distance (km)" : "Max distance (mi)") : "Counts";
    uint16_t lw = getTextWidth(label);
    uint16_t lx = box.x + (box.w - lw) / 2;
    uint16_t ly = box.y + box.h - SUBHEAD_DYUP;
    tft.setTextColor(RA8875_WHITE);
    tft.setCursor(lx, ly);
    tft.print(label);
}

/* calculate distance and azimuth between two points on the sphere.
 */
static void distSphere(LatLong p1, LatLong p2, int* km, float* az) {
    float co = cosf(p1.lng - p2.lng) * cosf(p1.lat) * cosf(p2.lat) + sinf(p1.lat) * sinf(p2.lat);
    float ca = acosf(co);
    *km = (int)(ca * ERAD_M * KM_PER_MI);

    float y = sinf(p2.lng - p1.lng) * cosf(p2.lat);
    float x = cosf(p1.lat) * sinf(p2.lat) - sinf(p1.lat) * cosf(p2.lat) * cosf(p2.lng - p1.lng);
    *az = rad2deg(atan2f(y, x));
    if (*az < 0) *az += 360;
}

/* reset all bstats
 */
static void resetBstats() {
    for (int i = 0; i < HAMBAND_N; i++) {
        bstats[i].count = 0;
        bstats[i].maxkm = 0;
        bstats[i].maxll = {};
    }
    // Also reset max indices
    for (int i = 0; i < HAMBAND_N; i++) spot_maxrpt[i] = -1;
}

/* Update bstats with a single spot's info.
 * idx is the index of this spot in reports[].
 */
static void updateBstats(const DXSpot& sp, int idx, bool of_de) {
    const HamBandSetting band = findHamBand(sp.kHz);
    if (band == HAMBAND_NONE) return;

    int km;
    float az;
    LatLong p1 = sp.rx_ll;  // To match original code logic: p1=rx, p2=tx for distance
    LatLong p2 = sp.tx_ll;
    distSphere(p1, p2, &km, &az);

    bstats[band].count++;

    // Logic from original: update maxkm
    // Original code: if (km > bstats[band].maxkm) ...
    // Note: Original code calculated distance differently for stats updates?
    // It used propDEPath which calculates distance from DE.
    // Wait, the original `distSphere` call in `retrievePSK` RBN section was:
    // distSphere(new_sp.rx_ll, new_sp.tx_ll, &km, &az);
    // bstats[band].count++;
    // if (km > bstats[band].maxkm) ...

    if (km > bstats[band].maxkm) {
        bstats[band].maxkm = km;
        bstats[band].maxll = of_de ? sp.rx_ll : sp.tx_ll;

        // Populate maxcall logic if needed, but bstats struct in pskreporter.cpp
        // implies we store maxcall there? The struct def is in HamClock.h probably.
        // Looking at original code, it copies call to bstats[band].maxcall.
        const char* call = of_de ? sp.rx_call : sp.tx_call;
        if (getSpotLabelType() == LBL_PREFIX)
            findCallPrefix(call, bstats[band].maxcall);
        else
            snprintf(bstats[band].maxcall, sizeof(bstats[band].maxcall), "%s", call);

        spot_maxrpt[band] = idx;
    }
}

/* Rebuild all bstats from the current reports list.
 */
static void rebuildBstats() {
    resetBstats();
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;
    for (int i = 0; i < n_reports; i++) {
        updateBstats(reports[i], i, of_de);
    }
}

/* retrieve spots into reports[] according to current settings.
 * return whether io ok.
 */
static bool retrievePSK(void) {
    // get fresh
    WiFiClient psk_client;
    bool ok = false;

    // query type
    bool ispsk = (psk_mask & PSKMB_SRCMASK) == PSKMB_PSK;
    bool iswspr = (psk_mask & PSKMB_SRCMASK) == PSKMB_WSPR;
    bool isrbn = (psk_mask & PSKMB_SRCMASK) == PSKMB_RBN;  // Should be false here if checkRBN works
    bool use_call = (psk_mask & PSKMB_CALL) != 0;
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;

    // If RBN, we handle it via checkRBN logic, so just return true (pretend OK)
    // so drawPSKPane() will be called by updatePSKReporter().
    if (isrbn) {
        return true;
    }

    // handy 4x DE maid if needed
    char de_maid[MAID_CHARLEN];
    getNVMaidenhead(NV_DE_GRID, de_maid);
    de_maid[4] = '\0';

    // reset lists
    n_reports = 0;
    resetBstats();

    // --------------------------------------------------------------------------
    // PSK: Direct XML Fetch
    // --------------------------------------------------------------------------
    if (ispsk) {
        char url[512];
        char t_buf[50];

        // Target
        if (use_call) {
            strncpy(t_buf, getCallsign(), sizeof(t_buf) - 1);
        } else {
            // If we are using Grid, we can use 4-char grid for query
            snprintf(t_buf, sizeof(t_buf), "%.4s", de_maid);
        }

        // Params
        char params[200];
        if (of_de) {
            // We are sender
            // If using grid, use senderLocator
            snprintf(params, sizeof(params), "%s=%s", use_call ? "senderCallsign" : "senderLocator", t_buf);
        } else {
            // We are receiver
            snprintf(params, sizeof(params), "%s=%s", use_call ? "receiverCallsign" : "receiverLocator", t_buf);
        }

        // Time: Absolute epoch or negative relative
        // We use absolute to be safe
        time_t start_t = myNow() - psk_maxage_mins * 60;

        snprintf(url, sizeof(url),
                 "https://retrieve.pskreporter.info/"
                 "query?%s&flowStartSeconds=%ld&rronly=1",
                 params, (long)start_t);

        Serial.printf("PSK: %s\n", url);

        const char* tmp_fn = "/tmp/hc_psk.xml";
        if (!curlDownload(url, tmp_fn)) {
            Serial.println("PSK: Download failed");
            goto out;
        }

        ok = true;
        updateClocks(false);

        FILE* fp = fopen(tmp_fn, "r");
        if (fp) {
            char line[4096];  // large buffer for XML
            while (fgets(line, sizeof(line), fp)) {
                // Find start of tag
                char* tag = strstr(line, "<receptionReport");
                if (!tag) continue;

                DXSpot new_sp = {};

                // Attribute parser helper
                auto getAttr = [&](const char* name, char* dst, size_t len) {
                    char key[50];
                    snprintf(key, sizeof(key), "%s=\"", name);
                    char* p = strstr(tag, key);
                    if (p) {
                        p += strlen(key);
                        char* end = strchr(p, '"');
                        if (end && (size_t)(end - p) < len) {
                            strncpy(dst, p, end - p);
                            dst[end - p] = 0;
                            return true;
                        }
                    }
                    return false;
                };

                // Extract fields
                getAttr("senderCallsign", new_sp.tx_call, sizeof(new_sp.tx_call));
                getAttr("senderLocator", new_sp.tx_grid, sizeof(new_sp.tx_grid));
                getAttr("receiverCallsign", new_sp.rx_call, sizeof(new_sp.rx_call));
                getAttr("receiverLocator", new_sp.rx_grid, sizeof(new_sp.rx_grid));
                getAttr("mode", new_sp.mode, sizeof(new_sp.mode));

                char nbuf[30];
                if (getAttr("frequency", nbuf, sizeof(nbuf))) new_sp.kHz = atof(nbuf) * 1e-3F;
                if (getAttr("sNR", nbuf, sizeof(nbuf))) new_sp.snr = atof(nbuf);
                if (getAttr("flowStartSeconds", nbuf, sizeof(nbuf))) new_sp.spotted = atol(nbuf);

                // Validate
                if (!maidenhead2ll(new_sp.tx_ll, new_sp.tx_grid)) continue;
                if (!maidenhead2ll(new_sp.rx_ll, new_sp.rx_grid)) continue;

                const HamBandSetting band = findHamBand(new_sp.kHz);
                if (band == HAMBAND_NONE) continue;

                if (!call2DXCC(new_sp.tx_call, new_sp.tx_dxcc)) continue;
                if (!call2DXCC(new_sp.rx_call, new_sp.rx_dxcc)) continue;

                // Add spot
                if (n_reports == n_malloced) {
                    reports = (DXSpot*)realloc(reports, (n_malloced += 100) * sizeof(DXSpot));
                    if (!reports) fatalError("PSK: no memory");
                }
                reports[n_reports] = new_sp;  // stored but count not inc yet

                // Update Stats relies on new spot being in reports[n_reports]
                updateBstats(new_sp, n_reports, of_de);

                n_reports++;
            }
            fclose(fp);
            unlink(tmp_fn);
        }

        goto out;
    }

    // --------------------------------------------------------------------------
    // WSPR: Legacy Logic
    // --------------------------------------------------------------------------

    if (iswspr) {
        char clean_query[1500];
        char full_url[2000];

        // Map HamBandSetting indices to WSPR band IDs
        static const int wspr_bands[] = {1, 3, 5, 7, 10, 14, 18, 21, 24, 28, 50, 144};
        char bands_str[100] = "";
        int b_count = 0;
        for (int i = 0; i < HAMBAND_N; i++) {
            if (TST_PSKBAND(i)) {
                char b_buf[10];
                snprintf(b_buf, sizeof(b_buf), "%d,", wspr_bands[i]);
                strcat(bands_str, b_buf);
                b_count++;
            }
        }
        if (b_count > 0)
            bands_str[strlen(bands_str) - 1] = '\0';  // remove trailing comma
        else
            strcpy(bands_str, "14");  // default 20m

        // build SQL query
        const char* role = of_de ? "tx" : "rx";
        snprintf(clean_query, sizeof(clean_query),
                 "SELECT "
                 "toUnixTimestamp(time),tx_loc,tx_sign,rx_loc,rx_sign,'WSPR',cast("
                 "frequency as UInt64),snr "
                 "FROM wspr.rx WHERE time > now() - %d AND band IN (%s) AND "
                 "(%s_sign = '%s' OR %s_loc LIKE '%.4s%%') "
                 "ORDER BY time DESC LIMIT 500 FORMAT CSV",
                 psk_maxage_mins * 60, bands_str, role, use_call ? getCallsign() : "NOSUCH", role,
                 use_call ? "NOSUCH" : de_maid);

        // URL encode query
        char* enc_query = (char*)malloc(3 * strlen(clean_query) + 1);
        if (!enc_query) fatalError("WSPR OOM");
        urlencode(enc_query, 3 * strlen(clean_query) + 1, clean_query);

        // full URL
        snprintf(full_url, sizeof(full_url), "http://db1.wspr.live/?query=%s", enc_query);
        free(enc_query);

        Serial.printf("WSPR: %s\n", full_url);

        // download to temp file
        const char* tmp_fn = "/tmp/hc_wspr.csv";
        if (curlDownload(full_url, tmp_fn)) {
            ok = true;
            updateClocks(false);

            FILE* fp = fopen(tmp_fn, "r");
            if (fp) {
                char line[200];
                while (fgets(line, sizeof(line), fp)) {
                    // Simple quote stripper for WSPR CSV
                    char *src = line, *dst = line;
                    while (*src) {
                        if (*src != '"') *dst++ = *src;
                        src++;
                    }
                    *dst = '\0';

                    // parse
                    DXSpot new_sp = {};
                    long posting_temp;
                    long Hz_temp;
                    if (sscanf(line, "%ld,%6[^,],%11[^,],%6[^,],%11[^,],%7[^,],%ld,%f", &posting_temp, new_sp.tx_grid,
                               new_sp.tx_call, new_sp.rx_grid, new_sp.rx_call, new_sp.mode, &Hz_temp,
                               &new_sp.snr) == 8) {
                        new_sp.spotted = posting_temp;
                        new_sp.kHz = Hz_temp * 1e-3F;

                        // convert grids to ll
                        if (maidenhead2ll(new_sp.tx_ll, new_sp.tx_grid) &&
                            maidenhead2ll(new_sp.rx_ll, new_sp.rx_grid)) {
                            // check for unknown or unsupported band
                            const HamBandSetting band = findHamBand(new_sp.kHz);
                            if (band != HAMBAND_NONE) {
                                // DXCC
                                if (call2DXCC(new_sp.tx_call, new_sp.tx_dxcc) &&
                                    call2DXCC(new_sp.rx_call, new_sp.rx_dxcc)) {
                                    // grow list if necessary
                                    if (n_reports == n_malloced) {
                                        reports = (DXSpot*)realloc(reports, (n_malloced += 100) * sizeof(DXSpot));
                                        if (!reports) fatalError("WSPR OOM");
                                    }
                                    reports[n_reports] = new_sp;  // store, count not inc yet

                                    // update stats
                                    updateBstats(new_sp, n_reports, of_de);

                                    n_reports++;
                                }
                            }
                        }
                    } else {
                        Serial.printf("WSPR: %s\n", line);
                    }
                }
                fclose(fp);
                unlink(tmp_fn);
            }
        } else {
            Serial.println("WSPR: download failed");
        }
    }

out:
    // reset counts if trouble
    if (!ok) {
        n_reports = 0;
        resetBstats();
    }

    // clean up
    updateClocks(false);
    psk_client.stop();
    return (ok);
}

// Check RBN for spots
void checkRBN(void) {
    bool isrbn = (psk_mask & PSKMB_SRCMASK) == PSKMB_RBN;

    // Disconnect if turned off
    if (!isrbn) {
        if (rbn_client) {
            rbn_client.stop();
            rbn_login_sent = false;
            rbn_filter_sent = false;
            // Clear reports immediately if we switch away from RBN?
            // Actually usually reports are cleared on mode switch by retrievePSK
        }
        return;
    }

    // Connect if needed
    if (!rbn_client || !rbn_client.connected()) {
        if (rbn_client.connect(rbn_host, rbn_port)) {
            Serial.println("RBN: connected");
            rbn_login_sent = false;
            rbn_filter_sent = false;
            rbn_last_activity = millis();

            // Re-init reports list for streaming mode (clear old)
            // But only if we were truly disconnected for a while?
            // Safer to clear to avoid stale data confusion
            n_reports = 0;
            resetBstats();
        } else {
            // connection failed, wait for next loop
            return;
        }
    }

    // Read incoming
    while (rbn_client.available()) {
        char line[200];
        if (!getTCPLine(rbn_client, line, sizeof(line), NULL)) break;

        rbn_last_activity = millis();

        // Login handling
        if (!rbn_login_sent) {
            if (strstr(line, "call:") || strstr(line, "login:")) {
                rbn_client.print(getCallsign());
                rbn_client.print("\r\n");
                rbn_login_sent = true;
                Serial.println("RBN: sent login");
            }
            continue;  // Wait for filter prompt or spots
        }

        // Filter handling - send once logged in (assumption: after login we see
        // prompt or spots) A simple heuristic: send filter after login, wait a bit
        if (rbn_login_sent && !rbn_filter_sent) {
            // Send filter based on settings
            // OFDE (My signal): User wants spots OF DE. "set dx filter call <mycall>"
            // !OFDE (I am skimmer): User wants spots BY DE. "set dx filter spotter
            // <mycall>"
            bool of_de = (psk_mask & PSKMB_OFDE) != 0;

            // If using Grid, we can't easily filter by Grid on RBN telnet
            // generically. We fallback to CALL based filter if possible, or accept
            // ALL and filter client side. Accepting ALL RBN is too much bandwidth. We
            // will enforce CALL filter even if Grid is selected, effectively mapping
            // Grid->Call preference which is what the user probably implies (their
            // call).

            if (of_de) {
                rbn_client.print("set dx filter call ");
                rbn_client.print(getCallsign());
                rbn_client.print("\r\n");
            } else {
                rbn_client.print("set dx filter spotter ");
                rbn_client.print(getCallsign());
                rbn_client.print("\r\n");
            }
            rbn_filter_sent = true;
            Serial.println("RBN: sent filter");
            continue;
        }

        // Parse Spot
        DXSpot spot;
        if (crackClusterSpot(line, spot)) {
            // Client-side filtering (Band, Logic)

            // Band check
            HamBandSetting band = findHamBand(spot.kHz);
            if (band == HAMBAND_NONE) continue;

            // Strict logic check
            bool of_de = (psk_mask & PSKMB_OFDE) != 0;
            bool use_call = (psk_mask & PSKMB_CALL) != 0;
            bool match = false;

            if (use_call) {
                if (of_de)
                    match = (strcmp(spot.tx_call, getCallsign()) == 0);
                else
                    match = (strcmp(spot.rx_call, getCallsign()) == 0);
            } else {
                // Grid check
                char de_maid[MAID_CHARLEN];
                getNVMaidenhead(NV_DE_GRID, de_maid);
                // Check first 4 chars
                if (of_de)
                    match = (strncmp(spot.tx_grid, de_maid, 4) == 0);
                else
                    match = (strncmp(spot.rx_grid, de_maid, 4) == 0);

                // Note: crackClusterSpot infers grid from Call if not present.
                // RBN spots might not have exact grid, so this matches the "inferred"
                // grid.
            }

            if (match) {
                // Add to list
                if (n_reports == n_malloced) {
                    // Alloc more
                    int new_n = n_malloced + 50;
                    reports = (DXSpot*)realloc(reports, new_n * sizeof(DXSpot));
                    if (!reports) fatalError("RBN OOM");
                    n_malloced = new_n;
                }
                reports[n_reports++] = spot;
                // Note: We don't update bstats here to avoid constant recalc.
                // bstats will be rebuilt in updatePSKReporter or lazily.
            }
        }
    }

    // Pruning old spots
    time_t now = myNow();
    time_t oldest = now - (psk_maxage_mins * 60);
    int head = 0;
    while (head < n_reports && reports[head].spotted < oldest) {
        head++;
    }
    if (head > 0) {
        // Shift payload
        int new_count = n_reports - head;
        memmove(reports, &reports[head], new_count * sizeof(DXSpot));
        n_reports = new_count;
    }

    // Send heartbeat
    if (millis() - rbn_last_activity > 60000) {
        rbn_client.print("\r\n");
        rbn_last_activity = millis();
    }
}

/* query PSK reporter etc for new reports, draw results and return whether all
 * ok
 */
bool updatePSKReporter(const SBox& box, bool force) {
    // save last retrieval settings to know whether reports[] can be reused
    static time_t next_update;           // don't update faster than PSK_INTERVAL
    static uint8_t my_psk_mask;          // setting used for reports[]
    static uint32_t my_psk_bands;        // setting used for reports[]
    static uint16_t my_psk_maxage_mins;  // setting used for reports[]
    static bool last_ok;                 // used to force retry

    bool isrbn = (psk_mask & PSKMB_SRCMASK) == PSKMB_RBN;

    if (isrbn) {
        // For RBN, we just redraw what we have from the background buffer
        // But we should rebuild bstats first to be sure they match reports[]
        rebuildBstats();
        drawPSKPane(box);
        return true;
    }

    // just use cache if settings all match and not too old
    if (!force && last_ok && reports && n_malloced > 0 && myNow() < next_update && my_psk_mask == psk_mask &&
        my_psk_maxage_mins == psk_maxage_mins && my_psk_bands == psk_bands) {
        drawPSKPane(box);
        return (true);
    }

    // save settings
    my_psk_mask = psk_mask;
    my_psk_maxage_mins = psk_maxage_mins;
    my_psk_bands = psk_bands;
    next_update = myNow() + PSK_INTERVAL;
    ;

    // get fresh
    last_ok = retrievePSK();

    // display whatever we got regardless
    drawPSKPane(box);

    // reply
    return (last_ok);
}

/* check for tap at s known to be within a PLOT_CH_PSK box.
 * return whether it was ours.
 */
bool checkPSKTouch(const SCoord& s, const SBox& box) {
    // done if tap title
    if (s.y < box.y + PANETITLE_H) return (false);

    // handy menu entry indices
    // N.B. must be in column-major order
    // N.B. keep in sync!
    enum {
        _M_RBN,
        _M_SPOT,
        _M_WHAT,
        _M_SHOW,
        _M_PATH,
        _M_AGE,
        _M_1HR,
        _M_160,
        _M_80,
        _M_60,
        _M_40,
        _M_PSK,
        _M_OFDE,
        _M_CALL,
        _M_DIST,
        _M_PON,
        _M_15M,
        _M_6HR,
        _M_30,
        _M_20,
        _M_17,
        _M_15,
        _M_WSPR,
        _M_BYDE,
        _M_GRID,
        _M_CNT,
        _M_POFF,
        _M_30M,
        _M_24H,
        _M_12,
        _M_10,
        _M_6,
        _M_2,
        _M_N
    };

    // handy current state
    bool ispsk = (psk_mask & PSKMB_SRCMASK) == PSKMB_PSK;
    bool iswspr = (psk_mask & PSKMB_SRCMASK) == PSKMB_WSPR;
    bool isrbn = (psk_mask & PSKMB_SRCMASK) == PSKMB_RBN;
    bool use_call = (psk_mask & PSKMB_CALL) != 0;
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;
    bool show_dist = psk_showdist != 0;
    bool show_path = psk_showpath != 0;

// menu
#define PRI_INDENT 2
#define SEC_INDENT 12
#define MI_N (HAMBAND_N + 21)  // ham_bands + controls
    MenuItem mitems[MI_N];

    if (MI_N != _M_N) fatalError("busted live spots menu size: %d != %d", MI_N, _M_N);

    // runMenu() expects column-major entries

    mitems[_M_RBN] = {MENU_1OFN, isrbn, 1, PRI_INDENT, "RBN", 0};
    mitems[_M_SPOT] = {MENU_LABEL, false, 0, PRI_INDENT, "Spot:", 0};
    mitems[_M_WHAT] = {MENU_LABEL, false, 0, PRI_INDENT, "What:", 0};
    mitems[_M_SHOW] = {MENU_LABEL, false, 0, PRI_INDENT, "Show:", 0};
    mitems[_M_PATH] = {MENU_LABEL, false, 0, PRI_INDENT, "Path:", 0};
    mitems[_M_AGE] = {MENU_LABEL, false, 0, PRI_INDENT, "Age:", 0};
    mitems[_M_1HR] = {MENU_1OFN, false, 6, 5, "1 hr", 0};
    mitems[_M_160] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_160M), 4, SEC_INDENT, findBandName(HAMBAND_160M), 0};
    mitems[_M_80] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_80M), 4, SEC_INDENT, findBandName(HAMBAND_80M), 0};
    mitems[_M_60] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_60M), 4, SEC_INDENT, findBandName(HAMBAND_60M), 0};
    mitems[_M_40] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_40M), 4, SEC_INDENT, findBandName(HAMBAND_40M), 0};

    mitems[_M_PSK] = {MENU_1OFN, ispsk, 1, PRI_INDENT, "PSK", 0};
    mitems[_M_OFDE] = {MENU_1OFN, of_de, 2, PRI_INDENT, "of DE", 0};
    mitems[_M_CALL] = {MENU_1OFN, use_call, 3, PRI_INDENT, "Call", 0};
    mitems[_M_DIST] = {MENU_1OFN, show_dist, 7, PRI_INDENT, "MaxDst", 0};
    mitems[_M_PON] = {MENU_1OFN, show_path, 8, PRI_INDENT, "On", 0};
    mitems[_M_15M] = {MENU_1OFN, false, 6, PRI_INDENT, "15 min", 0};
    mitems[_M_6HR] = {MENU_1OFN, false, 6, PRI_INDENT, "6 hrs", 0};
    mitems[_M_30] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_30M), 4, SEC_INDENT, findBandName(HAMBAND_30M), 0};
    mitems[_M_20] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_20M), 4, SEC_INDENT, findBandName(HAMBAND_20M), 0};
    mitems[_M_17] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_17M), 4, SEC_INDENT, findBandName(HAMBAND_17M), 0};
    mitems[_M_15] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_15M), 4, SEC_INDENT, findBandName(HAMBAND_15M), 0};

    mitems[_M_WSPR] = {MENU_1OFN, iswspr, 1, PRI_INDENT, "WSPR", 0};
    mitems[_M_BYDE] = {MENU_1OFN, !of_de, 2, PRI_INDENT, "by DE", 0};
    mitems[_M_GRID] = {MENU_1OFN, !use_call, 3, PRI_INDENT, "Grid", 0};
    mitems[_M_CNT] = {MENU_1OFN, !show_dist, 7, PRI_INDENT, "Count", 0};
    mitems[_M_POFF] = {MENU_1OFN, !show_path, 8, PRI_INDENT, "Off", 0};
    mitems[_M_30M] = {MENU_1OFN, false, 6, PRI_INDENT, "30 min", 0};
    mitems[_M_24H] = {MENU_1OFN, false, 6, PRI_INDENT, "24 hrs", 0};
    mitems[_M_12] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_12M), 4, SEC_INDENT, findBandName(HAMBAND_12M), 0};
    mitems[_M_10] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_10M), 4, SEC_INDENT, findBandName(HAMBAND_10M), 0};
    mitems[_M_6] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_6M), 4, SEC_INDENT, findBandName(HAMBAND_6M), 0};
    mitems[_M_2] = {MENU_AL1OFN, TST_PSKBAND(HAMBAND_2M), 4, SEC_INDENT, findBandName(HAMBAND_2M), 0};

    // set age
    switch (psk_maxage_mins) {
        case 15:
            mitems[_M_15M].set = true;
            break;
        case 30:
            mitems[_M_30M].set = true;
            break;
        case 60:
            mitems[_M_1HR].set = true;
            break;
        case 360:
            mitems[_M_6HR].set = true;
            break;
        case 1440:
            mitems[_M_24H].set = true;
            break;
        default:
            fatalError("Bad psk_maxage_mins: %d", psk_maxage_mins);
    }

    // create a box for the menu
    SBox menu_b;
    menu_b.x = box.x + 9;
    menu_b.y = box.y + 5;
    menu_b.w = 0;  // shrink to fit

    // run
    SBox ok_b;
    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_CANCELOK, 3, MI_N, mitems};
    if (runMenu(menu)) {
        // handy
        bool psk_set = mitems[_M_PSK].set;
        bool wspr_set = mitems[_M_WSPR].set;
        bool ofDE_set = mitems[_M_OFDE].set;
        bool call_set = mitems[_M_CALL].set;

        // RBN only works with ofcall -- No longer restricted!
        // But warning: RBN Grid support is weak.
        // We can allow it but it might not match well.
        // Legacy code restricted:
        // "RBN requires \"of DE\" and \"Call\""
        // We can remove this restriction if we trust our filter logic.
        // The previous restriction might have been due to proxie limitations or
        // query limits. Let's remove it and see. Or keep for safety if RBN volume
        // is too high. Actually, allowing "by DE" (skimmer) is useful.

        // Removing the "RBN requires..." check.

        if (false) {
            // dead code block to remove legacy check
        } else {
            // set new mode mask;
            psk_mask = psk_set ? PSKMB_PSK : (wspr_set ? PSKMB_WSPR : PSKMB_RBN);
            if (ofDE_set) psk_mask |= PSKMB_OFDE;
            if (call_set)
                psk_mask |= PSKMB_CALL;
            else
                psk_mask &= ~PSKMB_CALL;  // ensure cleared if not set (though logic
                                          // above implies toggle)

            // set new ham_bands
            psk_bands = 0;
            if (mitems[_M_160].set) SET_PSKBAND(HAMBAND_160M);
            if (mitems[_M_80].set) SET_PSKBAND(HAMBAND_80M);
            if (mitems[_M_60].set) SET_PSKBAND(HAMBAND_60M);
            if (mitems[_M_40].set) SET_PSKBAND(HAMBAND_40M);
            if (mitems[_M_30].set) SET_PSKBAND(HAMBAND_30M);
            if (mitems[_M_20].set) SET_PSKBAND(HAMBAND_20M);
            if (mitems[_M_17].set) SET_PSKBAND(HAMBAND_17M);
            if (mitems[_M_15].set) SET_PSKBAND(HAMBAND_15M);
            if (mitems[_M_12].set) SET_PSKBAND(HAMBAND_12M);
            if (mitems[_M_10].set) SET_PSKBAND(HAMBAND_10M);
            if (mitems[_M_6].set) SET_PSKBAND(HAMBAND_6M);
            if (mitems[_M_2].set) SET_PSKBAND(HAMBAND_2M);

            // get new age
            if (mitems[_M_15M].set)
                psk_maxage_mins = 15;
            else if (mitems[_M_30M].set)
                psk_maxage_mins = 30;
            else if (mitems[_M_1HR].set)
                psk_maxage_mins = 60;
            else if (mitems[_M_6HR].set)
                psk_maxage_mins = 360;
            else if (mitems[_M_24H].set)
                psk_maxage_mins = 1440;
            else
                fatalError("PSK: No menu age");

            // get how to show
            psk_showdist = mitems[_M_DIST].set;

            // get whether to show paths
            psk_showpath = mitems[_M_PON].set;

            // persist
            savePSKState();

            // refresh with new criteria
            updatePSKReporter(box, true);
        }
    }

    // ours alright
    return (true);
}

/* return current stats, if active
 */
bool getPSKBandStats(PSKBandStats stats[HAMBAND_N], const char* names[HAMBAND_N]) {
    if (findPaneForChoice(PLOT_CH_PSK) == PANE_NONE) return (false);

    // copy but zero out entries with 0 count
    memcpy(stats, bstats, sizeof(PSKBandStats) * HAMBAND_N);
    for (int i = 0; i < HAMBAND_N; i++) {
        if (bstats[i].count == 0) {
            stats[i].maxkm = 0;
            stats[i].maxll = {};
        }
        names[i] = findBandName((HamBandSetting)i);
    }

    return (true);
}

/* draw the current set of spot paths in reports[] if enabled
 */
void drawPSKPaths() {
    // ignore if not in any rotation set
    if (findPaneForChoice(PLOT_CH_PSK) == PANE_NONE) return;

    // which end to mark
    LabelOnMapEnd lom = (psk_mask & PSKMB_OFDE) ? LOME_RXEND : LOME_TXEND;

    if (psk_showdist) {
        // just show the longest path in each band
        for (int i = 0; i < HAMBAND_N; i++) {
            if (bstats[i].maxkm > 0 && TST_PSKBAND(i)) {
                if (psk_showpath) drawSpotPathOnMap(reports[spot_maxrpt[i]]);
                drawSpotLabelOnMap(reports[spot_maxrpt[i]], lom, LOMD_ALL);
            }
        }

    } else {
        // show all paths first
        if (psk_showpath) {
            for (int i = 0; i < n_reports; i++) {
                DXSpot& s = reports[i];
                if (TST_PSKBAND(findHamBand(s.kHz))) drawSpotPathOnMap(s);
            }
        }

        // then label all without text
        for (int i = 0; i < n_reports; i++) {
            // N.B. we know band in all reports[] are ok
            DXSpot& s = reports[i];
            if (TST_PSKBAND(findHamBand(s.kHz))) drawSpotLabelOnMap(s, LOME_BOTH, LOMD_JUSTDOT);
        }

        // then finally label only the farthest with text
        for (int i = 0; i < HAMBAND_N; i++)
            if (bstats[i].maxkm > 0 && TST_PSKBAND(i)) drawSpotLabelOnMap(reports[spot_maxrpt[i]], lom, LOMD_ALL);
    }
}

/* report spot closest to ll and which end to mark on map, if any within
 * MAX_CSR_DIST.
 */
bool getClosestPSK(LatLong& ll, DXSpot* sp, LatLong* mark_ll) {
    // ignore if not in any rotation set
    if (findPaneForChoice(PLOT_CH_PSK) == PANE_NONE) return (false);

    // which way?
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;

    if (psk_showdist) {
        // just check bstats if only showing farthest spots

        float min_d = 0;
        int min_i = -1;
        for (int i = 0; i < HAMBAND_N; i++) {
            if (TST_PSKBAND(i)) {
                float d = ll.GSD(bstats[i].maxll);
                if (min_i < 0 || d < min_d) {
                    min_d = d;
                    min_i = i;
                }
            }
        }

        if (min_i >= 0 && min_d * ERAD_M < MAX_CSR_DIST) {
            *sp = reports[spot_maxrpt[min_i]];
            *mark_ll = of_de ? sp->rx_ll : sp->tx_ll;
            return (true);
        }

    } else {
        // check all spots in displayed ham_bands.
        // N.B. can't use getClosestSpot() because of TST_PSKBAND

        float min_d = 0;
        int min_i = -1;
        for (int i = 0; i < n_reports; i++) {
            DXSpot& s = reports[i];
            if (TST_PSKBAND(findHamBand(s.kHz))) {
                float d = ll.GSD(s.rx_ll);
                if (min_i < 0 || d < min_d) {
                    min_d = d;
                    min_i = i;
                }
                d = ll.GSD(s.tx_ll);
                if (min_i < 0 || d < min_d) {
                    min_d = d;
                    min_i = i;
                }
            }
        }

        if (min_i >= 0 && min_d * ERAD_M < MAX_CSR_DIST) {
            *sp = reports[min_i];
            *mark_ll = of_de ? sp->rx_ll : sp->tx_ll;
            return (true);
        }
    }

    // none
    return (false);
}

/* if ms is over one of the bands in our pane report its info and where to mark
 * on map. return whether ms is really over any of our bands.
 */
bool getMaxDistPSK(const SCoord& ms, DXSpot* sp, LatLong* mark_ll) {
    // ignore if not currently up
    PlotPane pp = findPaneChoiceNow(PLOT_CH_PSK);
    if (pp == PANE_NONE) return (false);

    // which way?
    bool of_de = (psk_mask & PSKMB_OFDE) != 0;

    // find band where ms is located
    const SBox& box = plot_b[pp];
    SBox band_box;
    band_box.w = TBLCOLW;
    band_box.h = TBLROWH;
    for (int i = 0; i < HAMBAND_N; i++) {
        int row = i % (HAMBAND_N / 2);
        int col = i / (HAMBAND_N / 2);
        band_box.x = box.x + TBLHGAP + col * (TBLCOLW + TBLHGAP);
        band_box.y = box.y + LISTING_Y0 + row * TBLROWH;
        if (TST_PSKBAND(i) && inBox(ms, band_box) && bstats[i].maxkm > 0) {
            // report farthest spot on this band
            *sp = reports[spot_maxrpt[i]];
            *mark_ll = of_de ? sp->rx_ll : sp->tx_ll;
            return (true);
        }
    }

    return (false);
}

/* return PSKReports list
 */
void getPSKSpots(const DXSpot*& rp, int& n_rep) {
    rp = reports;
    n_rep = n_reports;
}

/* return drawing color for the given frequency, or black if not found.
 */
uint16_t getBandColor(float kHz) {
    HamBandSetting b = findHamBand(kHz);
    return (b != HAMBAND_NONE ? getMapColor(findColSel(b)) : RA8875_BLACK);
}

/* return whether the path for the given freq should be drawn dashed
 */
bool getBandPathDashed(float kHz) {
    HamBandSetting b = findHamBand(kHz);
    return (b != HAMBAND_NONE ? getPathDashed(findColSel(b)) : false);
}

/* return width to draw a map path for the given frequency.
 * returns 0 if band is turned off.
 */
int getRawBandPathWidth(float kHz) {
    HamBandSetting b = findHamBand(kHz);
    return (b != HAMBAND_NONE ? getRawPathWidth(findColSel(b)) : false);
}

/* return width to draw a map spot for the given frequency.
 * always returns the size even if the path color is turned off.
 */
int getRawBandSpotRadius(float kHz) {
    HamBandSetting b = findHamBand(kHz);
    return (b != HAMBAND_NONE ? getRawSpotRadius(findColSel(b)) : RAWWIDEPATHSZ);
}
