#!/system/bin/sh
# DISABLED: bridging logcat into /dev/kmsg re-enters logd (main/system) and
# pegs one guest core at ~100% regardless of LC:/TBXLC filters — brief tags
# the marker as the log tag, so substring filters miss. Use `adb logcat` or
# the serial console (printk) instead. Keep the service for init compatibility.
echo "<3>LCBRIDGE: disabled (avoids logd feedback spin)" > /dev/kmsg
exec sleep infinity
