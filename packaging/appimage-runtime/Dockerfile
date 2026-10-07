# C6.50: build environment of the AppImage Type-2 runtime.
# The base image is pinned by digest and every package by exact version
# (toolchain.lock, passed by build.sh). Nothing else is installed: the libraries
# that end up in the runtime are built from source by container-build.sh.
ARG OD_RT_IMAGE
FROM ${OD_RT_IMAGE}
ARG OD_RT_APK
RUN apk add --no-cache ${OD_RT_APK}
