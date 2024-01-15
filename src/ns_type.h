#pragma once

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef macro_max
    #define macro_max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef macro_min
    #define macro_min(a, b) ((a) < (b) ? (a) : (b))
#endif