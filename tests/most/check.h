/* Counted checks for the host C tests: a failure prints its message and the run goes on. */
#ifndef CARPLAY_TEST_CHECK_H
#define CARPLAY_TEST_CHECK_H

#include <stdarg.h>
#include <stdio.h>

static int checks, failures;

static void check(int ok, const char *format, ...) {
    va_list args;
    ++checks;
    if (ok) return;
    ++failures;
    printf("FAIL: ");
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

#endif
