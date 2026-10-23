#!/usr/bin/env bash
# drm_hwcomposer HWC3 is the CVD composer (thirdparty/common/drm_hwcomposer).
# The meson target links libui + HIDL composer@2.x-resources, which this NDK
# vendor image does not ship. Keep this script as the hook once those libs are
# extracted; until then hardware/graphics/stub provides IComposer v3 over KMS.
set -euo pipefail
echo "drm_hwcomposer NDK link is not wired (needs libui / HIDL composer resources)."
echo "Composer stays hardware/graphics/stub (KMS, same DRM path as drm_hwc)."
echo "Sources: thirdparty/common/drm_hwcomposer  rc/xml: $PWD"
exit 0
