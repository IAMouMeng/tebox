#pragma once

namespace gki {

/* Open /dev/vportN whose sysfs name matches |portName| (e.g. "tebox.aenc"). */
int openVirtioPort(const char* portName);

}  // namespace gki
