/* simple Serial.cpp
 */

#include "Serial.h"
#include "Arduino.h"

Serial::Serial(void) { pthread_mutex_init(&serial_lock, NULL); }

void Serial::begin(int baud) { (void)baud; }

void Serial::print(void) {}

void Serial::print(char c) { printf("%c", c); }

void Serial::print(char* s) { printf("%s", s); }

void Serial::print(const char* s) { printf("%s", s); }

void Serial::print(int i) { printf("%d", i); }

void Serial::print(String s) { printf("%s", s.c_str()); }

void Serial::println(void) { printf("\n"); }

void Serial::println(char* s) { printf("%s\n", s); }

void Serial::println(const char* s) { printf("%s\n", s); }

void Serial::println(int i) { printf("%d\n", i); }

#include <string.h>

extern bool verbose_logging;

int Serial::printf(const char* fmt, ...) {
    pthread_mutex_lock(&serial_lock);

    // format the message first to check content
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // default to QUIET (suppress logs).
    // if verbose_logging is set, print everything.
    // otherwise, only print if error/fail/fatal/panic
    if (!verbose_logging) {
        bool keep = false;
        if (strcasestr(buf, "error") || strcasestr(buf, "fail") || strcasestr(buf, "fatal") || strcasestr(buf, "panic"))
            keep = true;

        if (!keep) {
            pthread_mutex_unlock(&serial_lock);
            return n;
        }
    }

    // prefix with millis()
    // N.B. don't call now() because getNTPUTC calls print which can get recursive
    uint32_t m = millis();
    fprintf(stdout, "%7u.%03u ", m / 1000, m % 1000);

    // now the message
    fputs(buf, stdout);
    fflush(stdout);

    pthread_mutex_unlock(&serial_lock);

    // lint
    return (n);
}

Serial::operator bool() { return (true); }

class Serial Serial;
