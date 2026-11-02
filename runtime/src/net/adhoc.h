// Play with no server and no router: one console hosts the embedded server on
// the Vita's ad hoc network (the local network elsewhere), the others find it
// there. Either way the online settings of this launch then lead to it.
#pragma once

#include "status.h"

#include <functional>
#include <string>

namespace Adhoc {
using Progress = std::function<void(const std::string&)>;

Status Host(const Progress& progress);
Status Join(const Progress& progress);
} // namespace Adhoc
