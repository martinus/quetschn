# SPDX-License-Identifier: MIT OR GPL-2.0-only
# ClusterFuzzLite builds the fuzz targets in this image, see build.sh and .github/workflows/cflite.yml.
FROM gcr.io/oss-fuzz-base/base-builder
COPY . $SRC/seqlz
WORKDIR $SRC/seqlz
COPY .clusterfuzzlite/build.sh $SRC/
