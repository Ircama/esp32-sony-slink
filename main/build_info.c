/* When this image was built (compiled at every build: see CMakeLists.txt). */
#include "build_info.h"
const char *slink_build_date(void) { return __DATE__; }
const char *slink_build_time(void) { return __TIME__; }
