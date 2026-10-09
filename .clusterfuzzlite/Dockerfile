# SPDX-License-Identifier: MIT OR GPL-2.0-only
# ClusterFuzzLite builds the fuzz targets in this image, see build.sh and .github/workflows/cflite.yml.
FROM gcr.io/oss-fuzz-base/base-builder
COPY . $SRC/quetschn
WORKDIR $SRC/quetschn
COPY .clusterfuzzlite/build.sh $SRC/
