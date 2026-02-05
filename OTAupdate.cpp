/* handle remote firmware updating
 */

#include <ESP8266httpUpdate.h>
#include <ArduinoJson.h>

#include "HamClock.h"

// Release info structure
struct ReleaseInfo {
    char tag_name[32];
    char body[4096];
    char asset_url[256];
    bool valid;
};

// Helper to fetch latest release from GitHub
static ReleaseInfo getLatestRelease(void) {
    ReleaseInfo info;
    memset(&info, 0, sizeof(info));
    info.valid = false;

    // Use curl to fetch the GitHub API JSON
    // Limit to reasonable size to avoid buffer overflows if response is huge
    const char* cmd = "curl -L -s --max-time 10 https://api.github.com/repos/k4drw/hamclock/releases/latest";
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        Serial.println("OTA: Failed to run curl for version check");
        return info;
    }

    // Read response into a String/buffer
    // We allocation on heap to store the potentially large JSON
    const size_t buf_size = 16384;
    char* json_buf = (char*)malloc(buf_size);
    if (!json_buf) {
        pclose(fp);
        Serial.println("OTA: No memory for JSON buffer");
        return info;
    }

    size_t len = 0;
    while (len < buf_size - 1 && !feof(fp)) {
        size_t n = fread(json_buf + len, 1, buf_size - 1 - len, fp);
        if (n == 0) break;
        len += n;
    }
    json_buf[len] = '\0';
    pclose(fp);

    // Parse JSON
    DynamicJsonDocument doc(24576);  // Adjust size as needed
    DeserializationError error = deserializeJson(doc, json_buf);

    if (error) {
        Serial.printf("OTA: JSON parsing failed: %s\n", error.c_str());
    } else {
        const char* tag = doc["tag_name"];
        const char* body = doc["body"];

        // Find zip asset if available
        const char* download_url = NULL;
        JsonArray assets = doc["assets"];
        for (JsonObject asset : assets) {
            const char* name = asset["name"];
            if (name && strstr(name, ".zip")) {
                download_url = asset["browser_download_url"];
                break;
            }
        }
        // Fallback to source zipball if no specific asset
        if (!download_url) {
            download_url = doc["zipball_url"];
        }

        if (tag && download_url) {
            strncpy(info.tag_name, tag, sizeof(info.tag_name) - 1);
            if (body) strncpy(info.body, body, sizeof(info.body) - 1);
            strncpy(info.asset_url, download_url, sizeof(info.asset_url) - 1);
            info.valid = true;
        } else {
            Serial.println("OTA: JSON missing tag or download url");
        }
    }

    free(json_buf);
    return info;
}

// query layout
#define ASK_TO 60                        // ask timeout, secs
#define Q_Y 40                           // question y
#define C_Y 80                           // controls y
#define LH 30                            // line height
#define FD 7                             // font descent
#define LINDENT 10                       // list indent
#define INFO_Y 150                       // first list y
#define YNBOX_W 120                      // Y/N box width
#define YNBOX_H 40                       // Y/N box height
#define YNBOX_GAP 200                    // Y/N boxs gap
#define NBOX_X 50                        // no box x
#define NBOX_Y C_Y                       // no box y
#define YBOX_X (800 - NBOX_X - YNBOX_W)  // yes box x
#define YBOX_Y C_Y                       // yes box y
#define SCR_W 30                         // scroll width
#define SCR_M 5                          // scroll LR margin
#define SCR_X (800 - SCR_M - SCR_W - 5)  // scroll x
#define SCR_Y INFO_Y                     // scroll y
#define SCR_H (480 - 10 - SCR_Y)         // scroll height

// install layout
#define PROG_Y0 100                     // progress text y
#define PROG_DY 45                      // progress text line spacing
#define PBAR_INDENT 30                  // left and right progress bar indent
#define PBAR_Y0 200                     // progress bar top
#define PBAR_H 30                       // progress bar height
#define PBAR_W (800 - 2 * PBAR_INDENT)  // progress bar width

/* called by ESPhttpUpdate during download with bytes so far and total.
 */
static void onProgressCB(int sofar, int total) {
    tft.drawRect(PBAR_INDENT, PBAR_Y0, PBAR_W, PBAR_H, RA8875_WHITE);
    tft.fillRect(PBAR_INDENT, PBAR_Y0, sofar * PBAR_W / total, PBAR_H, RA8875_WHITE);
    checkWebServer(true);
}

// Helper to fetch version string from master branch version.cpp
static bool getMasterVersion(char* ver_out, int ver_len) {
    const char* cmd = "curl -s --max-time 10 https://raw.githubusercontent.com/k4drw/hamclock/master/version.cpp";
    FILE* fp = popen(cmd, "r");
    if (!fp) return false;

    char line[128];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        // Look for: const char *hc_version = "4.23"; or similar
        // Allow for spaces around = and other variations
        if (strstr(line, "hc_version")) {
            char* quote1 = strchr(line, '"');
            if (quote1) {
                char* quote2 = strchr(quote1 + 1, '"');
                if (quote2) {
                    *quote2 = '\0';
                    strncpy(ver_out, quote1 + 1, ver_len - 1);
                    ver_out[ver_len - 1] = '\0';
                    found = true;
                    break;
                }
            }
        }
    }
    pclose(fp);
    return found;
}

// Global to hold latest fetched info so we don't fetch twice
static ReleaseInfo latest_release;

/* return whether a new version is available.
 * if so pass back the name in new_ver[new_verl]
 * default no if error.
 */
bool newVersionIsAvailable(char* new_ver, uint16_t new_verl) {
    bool found_newer = false;
    char master_ver[32];

    // Check version.cpp on master
    if (getMasterVersion(master_ver, sizeof(master_ver))) {
        Serial.printf("OTA: Current %s, Master %s\n", hc_version, master_ver);

        float our_v = atof(hc_version);
        float new_v = atof(master_ver);

        bool we_are_beta = strchr(hc_version, 'b') != NULL;
        bool new_is_beta = strchr(master_ver, 'b') != NULL;

        if (we_are_beta) {
            if (new_v > our_v) {
                found_newer = true;
            }
            // Handle same numeric version but newer beta?
            // Since atof stops at 'b', 5.00b1 and 5.00b2 are both 5.00.
            // We need to parse suffix.
            else if (new_v == our_v) {
                int our_bv = atoi(strchr(hc_version, 'b') + 1);
                int new_bv = atoi(strchr(master_ver, 'b') + 1);
                if (new_bv > our_bv) found_newer = true;
            }
        } else {
            // Stable only accepts stable newer
            if (!new_is_beta && new_v > our_v + 0.001) {
                found_newer = true;
            }
        }

        if (found_newer) {
            strncpy(new_ver, master_ver, new_verl);
        }
    } else {
        Serial.println("OTA: Could not fetch master version.cpp");
    }

    return (found_newer);
}

/* draw as many of the given lines starting with top_line as will fit
 */
static void drawChangeList(char** line, int top_line, int n_lines) {
    uint16_t line_y = INFO_Y;

    // erase over to scroll bar
    tft.fillRect(0, line_y, SCR_X - SCR_M - 1, tft.height() - line_y, RA8875_BLACK);

    selectFontStyle(LIGHT_FONT, SMALL_FONT);
    tft.setTextColor(RA8875_WHITE);
    for (int i = top_line; i < n_lines && (line_y += LH) < tft.height() - FD; i++) {
        tft.setCursor(LINDENT, line_y);
        tft.print(line[i]);
    }
}

/* show release notes for current or pending release.
 * if pending:
 *    ask and return whether to install the given version using the given default answer
 * else
 *    always return false.
 */
bool askOTAupdate(char* new_ver, bool show_pending, bool def_yes) {
    // prep
    eraseScreen();
    hideClocks();
    selectFontStyle(BOLD_FONT, SMALL_FONT);
    tft.setTextColor(RA8875_WHITE);
    char line[128];

    // title
    tft.setCursor(LINDENT, Q_Y);
    if (show_pending)
        snprintf(line, sizeof(line), "New version %s is available. Update now?  ... ", new_ver);
    else
        snprintf(line, sizeof(line), "You're up to date with the following changes ... ");
    tft.print(line);

    // get cursor location for count down
    uint16_t count_x = tft.getCursorX();
    uint16_t count_y = tft.getCursorY();
    int count_s = ASK_TO;
    tft.print(count_s);

    // draw button boxes, no is just Ok if not pending
    SBox no_b = {NBOX_X, NBOX_Y, YNBOX_W, YNBOX_H};
    SBox yes_b = {YBOX_X, YBOX_Y, YNBOX_W, YNBOX_H};
    bool active_yes = def_yes;
    if (show_pending) {
        drawStringInBox("No", no_b, !active_yes, RA8875_WHITE);
        drawStringInBox("Yes", yes_b, active_yes, RA8875_WHITE);
    } else {
        drawStringInBox("Ok", no_b, false, RA8875_WHITE);
    }

    // prep for potentially long wait
    closeGimbal();
    closeDXCluster();

    // Use body from latest_release, or fetch if not present (only if this was called directly without
    // newVersionIsAvailable?) Typically newVersionIsAvailable processed it.
    if (!latest_release.valid) {
        latest_release = getLatestRelease();
    }

    // Split body into lines
    char** lines = NULL;
    int n_lines = 0;

    if (latest_release.valid && strlen(latest_release.body) > 0) {
        char* p = latest_release.body;
        char* brk;
        char* tok = strtok_r(p, "\n", &brk);
        while (tok) {
            // Handle wrapping
            // For now just add lines, assuming display wraps or we truncate
            // Actually existing code had wrap logic: maxStringW(line, SCR_X - SCR_M - LINDENT - 1);

            // Basic splitting for now
            lines = (char**)realloc(lines, (n_lines + 1) * sizeof(char*));
            lines[n_lines++] = strdup(tok);
            tok = strtok_r(NULL, "\n", &brk);
        }
    } else {
        lines = (char**)realloc(lines, sizeof(char*));
        lines[n_lines++] = strdup("No release notes available.");
    }

    // how many will fit
    const int max_lines = (tft.height() - FD - INFO_Y) / LH;

    // prep first display of changes
    drawChangeList(lines, 0, n_lines);

    // scrollbar
    SBox sb_b = {SCR_X, SCR_Y, SCR_W, SCR_H};
    ScrollBar sb;
    sb.init(max_lines, n_lines, sb_b);

    // prep for user input
    SBox screen_b = {0, 0, tft.width(), tft.height()};
    UserInput ui = {screen_b, UI_UFuncNone, UF_UNUSED, 1000, UF_NOCLOCKS, {0, 0}, TT_NONE, '\0', false, false};

    // wait for response or time out
    drainTouch();
    Serial.println("Waiting for update y/n ...");
    bool finished = false;
    while (!finished && count_s > 0) {
        // wait for any user action
        if (waitForUser(ui)) {
            // reset counter
            count_s = ASK_TO;

            if (sb.checkTouch(ui.kb_char, ui.tap)) {
                drawChangeList(lines, sb.getTop(), n_lines);

            } else {
                switch (ui.kb_char) {
                    case CHAR_TAB:
                    case CHAR_LEFT:
                    case CHAR_RIGHT:
                        if (show_pending) {
                            active_yes = !active_yes;
                            drawStringInBox("Yes", yes_b, active_yes, RA8875_WHITE);
                            drawStringInBox("No", no_b, !active_yes, RA8875_WHITE);
                        }
                        break;
                    case CHAR_ESC:
                        finished = true;
                        active_yes = false;
                        break;
                    case CHAR_CR:
                    case CHAR_NL:
                        finished = true;
                        break;

                    case CHAR_NONE:
                        // click?
                        if (show_pending && inBox(ui.tap, yes_b)) {
                            drawStringInBox("Yes", yes_b, true, RA8875_WHITE);
                            drawStringInBox("No", no_b, false, RA8875_WHITE);
                            wdDelay(200);
                            finished = true;
                            active_yes = true;
                        }
                        if (inBox(ui.tap, no_b)) {
                            if (show_pending) {
                                drawStringInBox("No", no_b, true, RA8875_WHITE);
                                drawStringInBox("Yes", yes_b, false, RA8875_WHITE);
                            } else {
                                drawStringInBox("Ok", no_b, true, RA8875_WHITE);
                            }
                            wdDelay(200);
                            finished = true;
                            active_yes = false;
                        }
                        break;
                }
            }
        }

        // update countdown
        tft.setTextColor(RA8875_WHITE);
        selectFontStyle(BOLD_FONT, SMALL_FONT);
        tft.fillRect(count_x, count_y - 30, 60, 40, RA8875_BLACK);
        tft.setCursor(count_x, count_y);
        tft.print(--count_s);
    }

    // clean up
    while (--n_lines >= 0) free(lines[n_lines]);
    free(lines);

    // return result
    Serial.printf("... update answer %d\n", active_yes);
    return (active_yes);
}

/* reload HamClock with the given version.
 * we never return regardless of success or fail.
 */
void doOTAupdate(const char* newver) {
    Serial.printf("Begin download version %s\n", newver);

    if (!latest_release.valid) {
        // Try fetch if missing
        latest_release = getLatestRelease();
    }

    // inform user
    eraseScreen();
    selectFontStyle(BOLD_FONT, SMALL_FONT);
    tft.setTextColor(RA8875_WHITE);
    tft.setCursor(0, PROG_Y0);
    tft.printf("  Performing remote update to V%s...", newver);
    tft.setCursor(0, PROG_Y0 + PROG_DY);
    tft.print("  Do not interrupt power or network during this process.");

    // connect progress callback
    ESPhttpUpdate.onProgress(onProgressCB);

    // build url from GitHub Assets
    WiFiClient client;
    const char* url = latest_release.valid ? latest_release.asset_url : NULL;

    if (!url) {
        fatalError("No download URL found.");
    }

    // go
    t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);

    // show error message and exit
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            fatalError("Update failed: Error %d\n%s\n", ESPhttpUpdate.getLastError(),
                       ESPhttpUpdate.getLastErrorString().c_str());
            break;

        case HTTP_UPDATE_NO_UPDATES:
            fatalError("No updates found after all??");
            break;

        case HTTP_UPDATE_OK:
            fatalError("Update Ok??");
            break;

        default:
            fatalError("Unknown failure code: %d", (int)ret);
            break;
    }
}
