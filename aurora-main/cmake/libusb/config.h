/* libusb build configuration for the Windows backend (MinGW/Clang). */
#pragma once

#define PLATFORM_WINDOWS 1
#define ENABLE_LOGGING 1
#define DEFAULT_VISIBILITY
#define HAVE_STRUCT_TIMESPEC 1
#define PRINTF_FORMAT(a, b) __attribute__((__format__(__printf__, a, b)))
