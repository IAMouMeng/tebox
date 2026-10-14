# Linux ARM64 runtime for CIX Sky1 hosts (MYT R1P): QEMU + VirGL on the CIX
# Mali GLES driver, VirGL video on the V4L2 mvxenc H.264 encoder, and
# tebox-api with host cast (tebox-cast) and UDP input.
# Built by scripts/r1p-deploy.sh from a staged context; only the final
# stage (binaries + launcher scripts, no sources) is shipped to the host.
# deps: the compilers and -dev packages only. r1p-deploy.sh tags it as
# tebox-r1p-builder so `r1p-deploy.sh bin` can rebuild tebox-cast in seconds.
FROM debian:trixie AS deps
ARG APT_MIRROR
RUN echo 'Acquire::Retries "8";' > /etc/apt/apt.conf.d/80-retries \
 && if [ -n "$APT_MIRROR" ]; then \
      sed -i "s|http://deb.debian.org|$APT_MIRROR|g" /etc/apt/sources.list.d/debian.sources; fi
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git ca-certificates python3 python3-venv python3-pip \
      ninja-build pkg-config flex bison \
      libglib2.0-dev libpixman-1-dev libsdl2-dev libslirp-dev libfdt-dev \
      libegl-dev libgles-dev libgl-dev libgbm-dev libdrm-dev libx11-dev \
      zlib1g-dev libzstd-dev libpng-dev libjpeg-dev linux-libc-dev \
      libavcodec-dev libavutil-dev libswscale-dev \
      rustc cargo bindgen libclang-dev libvulkan-dev glslang-tools \
    && rm -rf /var/lib/apt/lists/*
COPY .ci/requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install -q -r /tmp/requirements.txt
ENV PATH=/opt/venv/bin:$PATH

FROM deps AS build
# Two GPU backends: VirGL on Mali GLES and gfxstream (rutabaga) on Mali
# Vulkan, the default (boot-qemu.sh FORCE_RUTABAGA). Only what the QEMU
# build reads, so tebox-cast or launcher changes do not rebuild QEMU.
COPY .ci /src/.ci
COPY scripts/env.sh /src/scripts/env.sh
COPY thirdparty /src/thirdparty
COPY qemu /src/qemu
WORKDIR /src
RUN bash .ci/build-libepoxy.sh \
 && VIRGL_VIDEO=1 VIRGL_VIDEO_BACKEND=v4l2 bash .ci/build-virglrenderer.sh \
 && bash .ci/build-rutabaga.sh \
 && RUTABAGA=1 VIRGL_VIDEO=1 QEMU_INSTALL=1 bash .ci/build-qemu.sh
# Host cast encoder: direct V4L2 (mvxenc), Debian's FFmpeg as fallback.
COPY src/service/cast /src/src/service/cast
RUN make -C src/service/cast CAST_SYSTEM_FFMPEG=1
# Collect the shared libraries QEMU needs, except libc and the GL/EGL/GBM
# stack, which must come from the CIX userspace driver at runtime.
RUN set -e; P=/src/prebuilts/host/linux-arm64; mkdir -p /out/lib; \
    LD_LIBRARY_PATH=$P/libepoxy/lib:$P/virglrenderer/lib:$P/rutabaga/lib \
      ldd $P/qemu/bin/qemu-system-aarch64 /src/out/service/tebox-cast \
      | awk '/=> \//{print $3}' | sort -u \
      | grep -vE '/(libc|libm|libdl|libpthread|librt|ld-linux[^/]*|libEGL[^/]*|libGL[^/]*|libGLES[^/]*|libgbm|libGLdispatch|libGLX)\.so' \
      | xargs -r cp -L -t /out/lib/

FROM debian:trixie
ARG APT_MIRROR
RUN echo 'Acquire::Retries "8";' > /etc/apt/apt.conf.d/80-retries \
 && if [ -n "$APT_MIRROR" ]; then \
      sed -i "s|http://deb.debian.org|$APT_MIRROR|g" /etc/apt/sources.list.d/debian.sources; fi
RUN apt-get update && apt-get install -y --no-install-recommends \
      python3-minimal e2fsprogs procps iproute2 adb \
      libdrm2 libwayland-client0 libwayland-server0 \
      libvulkan1 vulkan-tools \
    && rm -rf /var/lib/apt/lists/*
# Every package from thirdparty/host-drivers/<platform> (r1p-deploy.sh).
COPY host-drivers/ /tmp/drivers/
# Only /opt is needed: the debs also carry ./lib (would break usrmerge),
# udev/systemd units and firmware that belong to the host, not the container.
# Plus the Vulkan ICD manifest (gfxstream renders on Mali Vulkan).
RUN for d in /tmp/drivers/*.deb; do dpkg -x "$d" /tmp/drivers/root; done \
 && cp -a /tmp/drivers/root/opt/. /opt/ \
 && if [ -d /tmp/drivers/root/etc/vulkan/icd.d ]; then \
      mkdir -p /etc/vulkan/icd.d && cp -a /tmp/drivers/root/etc/vulkan/icd.d/. /etc/vulkan/icd.d/; fi \
 && rm -rf /tmp/drivers
ARG VARIANT
ARG KERNEL_ID
ENV TEBOX=/opt/tebox VARIANT=${VARIANT} KERNEL_ID=${KERNEL_ID}
COPY --from=build /src/prebuilts/host/linux-arm64 /opt/tebox/prebuilts/host/linux-arm64
COPY --from=build /out/lib /opt/tebox/lib
COPY --from=build /src/out/service/tebox-cast /opt/tebox/out/service/tebox-cast
COPY scripts/env.sh scripts/boot-qemu.sh scripts/check-runtime-inputs.py \
     scripts/cix-mali-env.sh scripts/cix-r1p-start.sh /opt/tebox/scripts/
# tebox-api (cross-built by r1p-deploy.sh) starts instances through ./run.
COPY tebox-api /opt/tebox/bin/tebox-api
COPY scrcpy/ /opt/tebox/prebuilts/android-arm64/scrcpy/
RUN printf '#!/bin/sh\nexec /opt/tebox/scripts/boot-qemu.sh "$@"\n' > /opt/tebox/run \
 && chmod +x /opt/tebox/run
COPY runtime-src/ /opt/tebox/src/
ENTRYPOINT ["/opt/tebox/scripts/cix-r1p-start.sh"]
