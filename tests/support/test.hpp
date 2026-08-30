#pragma once

#include <iostream>

#define MIRA_CHECK(condition)                                                                    \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #condition << '\n'; \
            return 1;                                                                            \
        }                                                                                        \
    } while (false)
