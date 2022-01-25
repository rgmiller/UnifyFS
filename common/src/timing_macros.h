// Some macros for recording the runtime of various functions
// This might eventually get checked in, but I don't actually expect it to.
//
// RGM - 14 Jan 2022


// Basically, we're defining 2 macros: one that is pasted at the top of a
// function we want to instrument and one that is pasted at the bottom.  The
// macros will record the time in between them and log that using UnifyFS's
// standard logging functionality.

// Note: If your function returns from anywhere but the end, you won't get
// any elapsed time info in your logs.  The program shouldn't break, however.

#ifndef __TIMING_MACROS_H__
#define __TIMING_MACROS_H__

// Define this macro to enable the timing stuff
// Comment out to disable everything.
#define TIMING_ENABLED  

#ifdef TIMING_ENABLED
#include <sys/time.h>

// Calculate elapsed time - returns microseconds
// NOTE: This needs to be a fast function, so it skips a few basic checks
// such as whether tStop actually is later than tStart.  Also, on 32-bit
// systems, there's a max time of about 4200 seconds (~70 minutes) because
// it uses 32 bit uints and converts seconds immediately to microseconds.
static inline unsigned long
elapsedTimeForTimingMacros( const struct timeval *start, const struct timeval *stop)
// Uses an excessively long name just to make sure we don't collide with
// any other elapsed time functions that might exist somewhere in the code
{
    unsigned long rv;
    if (start->tv_sec == stop->tv_sec) {
        rv = stop->tv_usec - start->tv_usec;
    } else {
        rv = (stop->tv_sec - start->tv_sec) * 1000000;
        rv -= start->tv_usec;
        rv += stop->tv_usec;
    }

    return rv;
}


#define TIMING_TOP() \
    LOGDBG("**TIMING**: At top of %s", __func__); \
    struct timeval start_time; \
    gettimeofday( &start_time, NULL);


// Must be placed before the final return statement!
#define TIMING_BOT() \
    struct timeval end_time; \
    gettimeofday( &end_time, NULL); \
    float elapsed = elapsedTimeForTimingMacros( &start_time, &end_time) / 1000000.0; \
    LOGDBG("**TIMING**: At bottom of %s - elapsed time: %f (s)", __func__, elapsed)

#else
    // Timing macros disabled
    #define TIMING_TOP()
    #define TIMING_BOT()
#endif // 

#endif // __TIMING_MACROS_H__