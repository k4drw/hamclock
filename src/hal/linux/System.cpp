#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "Arduino.h"

/* return milliseconds since first call
 */
uint32_t millis(void) {
    static struct timeval t0;
    struct timeval t;
    gettimeofday(&t, NULL);
    if (t0.tv_sec == 0) t0 = t;
    uint32_t dt_ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_usec - t0.tv_usec) / 1000;
    return (dt_ms);
}

void delay(uint32_t ms) { usleep(ms * 1000); }

long random(int max) { return ((::random() >> 3) % max); }

void randomSeed(int s) { ::srandom(s); }

uint16_t analogRead(int pin) {
    (void)pin;
    return (0);
}

/* libcurl integration */
#include <curl/curl.h>

static size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

bool curlDownload(const char* url, const char* filename) {
    CURL* curl;
    FILE* fp;
    CURLcode res;
    bool ok = false;

    // Global init is technically needed once, but let's assume HamClock or standard curl auto-handling works.
    // Ideally call curl_global_init(CURL_GLOBAL_ALL) in main.cpp, but lazy init often works or we do it here safely?
    // curl_easy_init handles it if not done? Not guaranteed thread-safe if done lazily multiple times.
    // HamClock runs this in main thread usually.

    curl = curl_easy_init();
    if (curl) {
        fp = fopen(filename, "wb");
        if (fp) {
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);  // 15 sec timeout
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "HamClock/4.30");
            // Optional: verbose
            // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

            res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                ok = true;
            } else {
                printf("curlDownload failed: %s\n", curl_easy_strerror(res));
            }
            fclose(fp);
            if (!ok) unlink(filename);
        } else {
            printf("curlDownload: cannot open %s\n", filename);
        }
        curl_easy_cleanup(curl);
    }
    return ok;
}
