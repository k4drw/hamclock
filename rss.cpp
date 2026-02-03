/* manage displaying RSS feeds or local RESTful content.
 */

#include "HamClock.h"

// global state
uint8_t rss_interval = RSS_DEF_INT;  // polling period, secs
SBox rss_bnr_b;                      // main rss banner over map
uint8_t rss_on;                      // whether currently on
bool rss_local;                      // if set: use local titles, not server

// local state
#define RSS_MAXN 30                       // max number RSS entries to cache
static char* rss_titles[RSS_MAXN];        // malloced titles
static char* rss_tapurl;                  // malloced URL if tapped, if any
static uint8_t rss_ntitles, rss_title_i;  // n titles and rolling index
static time_t rss_next;                   // when to retrieve next set

/* simple HTML/XML tag stripper and entity decoder.
 * dst must be at least as large as src.
 */
static void cleanupText(char* dst, const char* src) {
    bool in_tag = false;
    char* d = dst;
    const char* s = src;

    while (*s) {
        if (*s == '<') {
            in_tag = true;
        } else if (*s == '>') {
            in_tag = false;
        } else if (!in_tag) {
            // handle entities
            if (*s == '&') {
                if (strncmp(s, "&amp;", 5) == 0) {
                    *d++ = '&';
                    s += 4;
                } else if (strncmp(s, "&lt;", 4) == 0) {
                    *d++ = '<';
                    s += 3;
                } else if (strncmp(s, "&gt;", 4) == 0) {
                    *d++ = '>';
                    s += 3;
                } else if (strncmp(s, "&quot;", 6) == 0) {
                    *d++ = '"';
                    s += 5;
                } else if (strncmp(s, "&apos;", 6) == 0) {
                    *d++ = '\'';
                    s += 5;
                } else if (strncmp(s, "&nbsp;", 6) == 0) {
                    *d++ = ' ';
                    s += 5;
                } else
                    *d++ = *s;
            } else if (*s == '\n' || *s == '\r' || *s == '\t') {
                // normalize whitespace to space
                if (d > dst && d[-1] != ' ') *d++ = ' ';
            } else {
                *d++ = *s;
            }
        }
        s++;
    }
    *d = 0;
}

/* add a title to the list, formatting as "Source: Title"
 */
static void addTitle(const char* src, const char* raw_title) {
    if (rss_ntitles >= RSS_MAXN) return;
    if (!raw_title || !*raw_title) return;

    char clean[512];
    cleanupText(clean, raw_title);

    // trim leading/trailing spaces
    char* start = clean;
    while (*start == ' ') start++;
    char* end = start + strlen(start) - 1;
    while (end > start && *end == ' ') *end-- = 0;

    if (strlen(start) < 3) return;

    // avoid dupes
    char combined[600];
    snprintf(combined, sizeof(combined), "%s: %s", src, start);

    for (int i = 0; i < rss_ntitles; i++) {
        if (strcmp(rss_titles[i], combined) == 0) return;
    }

    rss_titles[rss_ntitles++] = strdup(combined);
}

/* Parse HamWeekly Atom feed
 * Look for <entry> ... <title>...</title>
 */
static void parseHamWeekly(const char* fn) {
    FILE* fp = fopen(fn, "r");
    if (!fp) return;

    char line[1024];
    bool in_entry = false;
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < 5) {
        if (strstr(line, "<entry>")) in_entry = true;
        if (in_entry) {
            char* title_start = strstr(line, "<title>");
            if (title_start) {
                title_start += 7;
                char* title_end = strstr(title_start, "</title>");
                if (title_end) {
                    *title_end = 0;
                    addTitle("HamWeekly.com", title_start);
                    count++;
                    in_entry = false;  // assumes one title per entry and we want the first
                }
            }
        }
    }
    fclose(fp);
}

/* Parse ARNewsLine RSS
 * Look for first <item>, then extract bullet points from description/content
 */
static void parseARNewsLine(const char* fn) {
    FILE* fp = fopen(fn, "r");
    if (!fp) return;

    // Read entire file into buffer (it's text, usually < 50KB) to handle
    // multiline content
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize > 200000) fsize = 200000;  // cap size
    char* buf = (char*)malloc(fsize + 1);
    if (!buf) {
        fclose(fp);
        return;
    }
    fread(buf, 1, fsize, fp);
    buf[fsize] = 0;
    fclose(fp);

    // Find first item
    char* item = strstr(buf, "<item>");
    if (item) {
        // Find content
        char* content = strstr(item, "<description>");  // simple RSS usually has description
        // Perl script checks content body. ARNewsLine often puts full text in
        // description or encoded.
        if (!content) content = strstr(item, "<content:encoded>");

        if (content) {
            // Find closing tag
            char* end = strchr(content, '>');
            if (end) {
                content = end + 1;
                // scan for bullets "- "
                // The perl script splits by newline and regexes ^\s*-\s*(.+)

                char* p = content;
                int count = 0;
                while (*p && count < 5) {
                    if (*p == '-') {
                        // check if it looks like a bullet
                        char* line_end = strchr(p, '\n');
                        if (!line_end) line_end = strchr(p, '<');  // or tag start
                        if (!line_end) break;

                        // Extract text
                        int len = line_end - p;
                        if (len > 5 && len < 200) {
                            char bullet[200];
                            strncpy(bullet, p + 1, len - 1);  // skip '-'
                            bullet[len - 1] = 0;

                            // check if valid text
                            addTitle("ARNewsLine.org", bullet);
                            count++;
                        }
                        p = line_end;
                    }
                    p++;
                    // Limit search to not go into next item
                    if (strncmp(p, "</item>", 7) == 0) break;
                }
            }
        }
    }
    free(buf);
}

/* Parse NG3K HTML
 * Look for <tr class="adxoitem"> ...
 */
static void parseNG3K(const char* fn) {
    FILE* fp = fopen(fn, "r");
    if (!fp) return;

    char line[2048];  // rows can be long
    int count = 0;

    // We scan line by line. This depends on the HTML structure being somewhat
    // line-oriented or at least containing the markers we need.
    while (fgets(line, sizeof(line), fp) && count < 5) {
        if (strstr(line, "class=\"adxoitem\"")) {
            // Found a row. We need to extract fields.
            // Format: <td>DATE</td> <td>DATE</td> <td class="cty">ENTITY</td> ...
            // <span class="call">CALL</span> This is hard with simple strstr. Let's
            // try a heuristic: find "cty">, "call">

            char* cty = strstr(line, "class=\"cty\">");
            char* call = strstr(line, "class=\"call\">");
            char* date = strstr(line, "class=\"date\">");  // appears twice

            if (cty && call && date) {
                // Extract Entity
                char ent_buf[50] = {0};
                cty += 12;
                char* cty_end = strchr(cty, '<');
                if (cty_end && (size_t)(cty_end - cty) < sizeof(ent_buf)) {
                    strncpy(ent_buf, cty, cty_end - cty);
                }

                // Extract Call
                char call_buf[50] = {0};
                call += 13;
                char* call_end = strchr(call, '<');
                if (call_end && (size_t)(call_end - call) < sizeof(call_buf)) {
                    strncpy(call_buf, call, call_end - call);
                }

                if (ent_buf[0] && call_buf[0]) {
                    char title[150];
                    snprintf(title, sizeof(title), "%s: %s", ent_buf, call_buf);
                    addTitle("NG3K.com", title);
                    count++;
                }
            }
        }
    }
    fclose(fp);
}

/* download more RSS titles.
 * return whether io ok, even if no new titles.
 */
static bool retrieveRSS(void) {
    // reset count and index
    rss_ntitles = rss_title_i = 0;

    // 1. HamWeekly
    const char* url_hw = "https://daily.hamweekly.com/atom.xml";
    const char* tmp_hw = "/tmp/rss_hw.xml";
    if (curlDownload(url_hw, tmp_hw)) {
        parseHamWeekly(tmp_hw);
        unlink(tmp_hw);
    }

    // 2. ARNewsLine
    const char* url_ar = "https://www.arnewsline.org/?format=rss";
    const char* tmp_ar = "/tmp/rss_ar.xml";
    if (curlDownload(url_ar, tmp_ar)) {
        parseARNewsLine(tmp_ar);
        unlink(tmp_ar);
    }

    // 3. NG3K
    const char* url_ng = "https://www.ng3k.com/Misc/adxo.html";
    const char* tmp_ng = "/tmp/rss_ng.html";
    if (curlDownload(url_ng, tmp_ng)) {
        parseNG3K(tmp_ng);
        unlink(tmp_ng);
    }

    // If we failed to get anything, try to recover old ones if memory wasn't
    // freed? No, we overwrote the array. But entries are malloced. Clean up old
    // entries that might be dangling if we reused the array? Actually, we reset
    // rss_ntitles to 0. We should free old ones first. Note: The loop below frees
    // everything before fetch if !local. But since we do it here:

    return rss_ntitles > 0;
}

/* display next RSS feed item if on, retrieving more as needed.
 * if local always return true, else return whether retrieval io was ok.
 */
static bool updateRSS(void) {
    // skip if not on and clear list if not local
    if (!rss_on) {
        if (!rss_local) {
            while (rss_ntitles > 0) {
                free(rss_titles[--rss_ntitles]);
                rss_titles[rss_ntitles] = NULL;
            }
        }
        return (true);
    }

    // prepare background to show life before possibly lengthy net update
    fillSBox(rss_bnr_b, RSS_BG_COLOR);
    tft.drawLine(rss_bnr_b.x, rss_bnr_b.y, rss_bnr_b.x + rss_bnr_b.w, rss_bnr_b.y, GRAY);

    // fill rss_titles[] from network if empty and wanted
    if (!rss_local && rss_title_i >= rss_ntitles) {
        // Clean up old titles first
        for (int i = 0; i < RSS_MAXN; i++) {
            if (rss_titles[i]) {
                free(rss_titles[i]);
                rss_titles[i] = NULL;
            }
        }
        rss_ntitles = 0;

        bool ok = retrieveRSS();

        // display err msg if still no rss_titles
        if (!ok || rss_ntitles == 0) {
            selectFontStyle(LIGHT_FONT, SMALL_FONT);
            const char* msg = ok ? "No RSS titles" : "RSS network error";
            uint16_t msg_w = getTextWidth(msg);
            tft.setTextColor(RSS_FG_COLOR);
            tft.setCursor(rss_bnr_b.x + (rss_bnr_b.w - msg_w) / 2, rss_bnr_b.y + 2 * rss_bnr_b.h / 3 - 1);
            tft.print(msg);
            Serial.printf("RSS: %s\n", msg);
            return (ok);
        }

        rss_title_i = 0;
    }

    // done if no titles
    if (rss_ntitles == 0) return (true);

    // draw next rss_title
    char* title = rss_titles[rss_title_i];

    // usable banner drawing x and width
    uint16_t ubx = rss_bnr_b.x + 5;
    uint16_t ubw = rss_bnr_b.w - 10;

    // get title width in pixels
    selectFontStyle(LIGHT_FONT, SMALL_FONT);
    int tw = getTextWidth(title);

    // draw as 1 or 2 lines to fit within ubw
    tft.setTextColor(RSS_FG_COLOR);
    if (tw < ubw) {
        // title fits on one row, draw centered horizontally and vertically
        tft.setCursor(ubx + (ubw - tw) / 2, rss_bnr_b.y + 2 * rss_bnr_b.h / 3 - 1);
        tft.print(title);
    } else {
        // title too long, keep shrinking until it fits
        for (bool fits = false; !fits;) {
            // split at center blank
            int tl = strlen(title);
            char* row2 = strchr(title + tl / 2, ' ');
            if (!row2) row2 = title + tl / 2;  // no blanks! just split in half?
            char sep_char = *row2;             // save to restore
            *row2++ = '\0';                    // replace blank with EOS and move to start of row 2 -- restore!
            uint16_t r1w = getTextWidth(title);
            uint16_t r2w = getTextWidth(row2);

            // draw if fits
            if (r1w <= ubw && r2w <= ubw) {
                tft.setCursor(ubx + (ubw - r1w) / 2, rss_bnr_b.y + rss_bnr_b.h / 2 - 8);
                tft.print(title);
                tft.setCursor(ubx + (ubw - r2w) / 2, rss_bnr_b.y + rss_bnr_b.h - 9);
                tft.print(row2);

                // got it
                fits = true;
            }

            // restore zerod char
            row2[-1] = sep_char;

            if (!fits) {
                Serial.printf("RSS shrink from %d %d ", tw, tl);
                tw = maxStringW(title, 9 * tw / 10);  // modifies title
                tl = strlen(title);
                Serial.printf("to %d %d\n", tw, tl);
            }
        }
    }

    // reset url for tap, restore if available
    free(rss_tapurl);
    rss_tapurl = NULL;

    // if local just cycle to next title, else remove from list and advance
    if (rss_local) {
        rss_title_i = (rss_title_i + 1) % rss_ntitles;
    } else {
        // before removing current title, save up to : in rss_tapurl in case of tap
        // Original logic: "Source: Title" -> https://Source
        // We use Sources like "HamWeekly.com" and "ARNewsLine.org"
        char* title = rss_titles[rss_title_i];
        char* colon = strchr(title, ':');
        if (colon) {
            char url[200];
            snprintf(url, sizeof(url), "https://%.*s", (int)(colon - title), title);
            rss_tapurl = strdup(url);
        }

        // finished with this title
        free(rss_titles[rss_title_i]);
        rss_titles[rss_title_i++] = NULL;
    }

    return (true);
}

/* called frequently to check for RSS updates
 */
void checkRSS(void) {
    if (myNow() >= rss_next) {
        if (updateRSS())
            rss_next = myNow() + rss_interval;
        else
            rss_next = nextWiFiRetry("RSS");
    }
}

/* used by web server to control local RSS title list.
 * if title == NULL
 *   restore normal network operation
 * else if title[0] == '\0'
 *   turn off network and empty the local title list
 * else
 *   turn off network and add the given title to the local list
 * always report the current number of titles in the list and max number
 * possible. return whether ok
 */
bool setRSSTitle(const char* title, int& n_titles, int& max_titles) {
    if (!title) {
        // restore network operation
        rss_local = false;
        rss_ntitles = rss_title_i = 0;

    } else {
        // erase list if network on or asked to do so
        if (!rss_local || title[0] == '\0') {
            rss_ntitles = rss_title_i = 0;
            for (int i = 0; i < RSS_MAXN; i++) {
                if (rss_titles[i]) {
                    free(rss_titles[i]);
                    rss_titles[i] = NULL;
                }
            }
        }

        // turn off network
        rss_local = true;

        // add title if room unless blank
        if (title[0] != '\0') {
            if (rss_ntitles < RSS_MAXN) {
                if (rss_titles[rss_ntitles])  // just paranoid
                    free(rss_titles[rss_ntitles]);
                rss_titles[rss_ntitles] = strdup(title);
                rss_title_i = rss_ntitles++;  // show new title
            } else {
                n_titles = RSS_MAXN;
                max_titles = RSS_MAXN;
                return (false);
            }
        }
    }

    // update info and refresh
    n_titles = rss_ntitles;
    max_titles = RSS_MAXN;
    scheduleRSSNow();

    // ok
    return (true);
}

/* called when RSS has just been turned on: update now and restart refresh cycle
 */
void scheduleRSSNow() { rss_next = 0; }

/* open the web page associated with the current title, if any
 */
void checkRSSTouch(void) {
    if (rss_tapurl) openURL(rss_tapurl);
}
