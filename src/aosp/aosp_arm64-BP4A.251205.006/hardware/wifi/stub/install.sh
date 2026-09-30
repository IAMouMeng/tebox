#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../../.." && pwd)
AOSP=$(cd "$HERE/../../.." && pwd)
STUB="$AOSP/qemu/vendor"
install -m 0755 "$ROOT/out/wifi-stub/bin/android.hardware.wifi-service" "$STUB/bin/hw/"
install -m 0644 "$HERE/../init/android.hardware.wifi-service.rc" "$STUB/etc/init/"
install -m 0644 "$HERE/../vintf/android.hardware.wifi-service.xml" "$STUB/etc/vintf/manifest/"
python3 - "$STUB/manifest.xml" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
text = p.read_text()
if '<name>android.hardware.wifi</name>' not in text:
    entry = '''    <hal format="aidl">
        <name>android.hardware.wifi</name>
        <version>2</version>
        <fqname>IWifi/default</fqname>
    </hal>
'''
    p.write_text(text.replace('</manifest>', entry + '</manifest>'))
PY
