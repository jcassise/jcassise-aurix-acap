# Usage: docker build --build-arg SDK_IMAGE=axisecp/acap-native-sdk:<tag> --build-arg ARCH=<armv7hf|aarch64> -t aurix .
# The SDK tag decides the minimum AXIS OS; ARCH picks the matching manifest schema:
#   armv7hf -> SDK 1.15, schema 1.7.0 (AXIS OS 11.11)
#   aarch64 -> SDK 12.11, schema 2.2.0 (AXIS OS 12.11, ready for 13)
ARG SDK_IMAGE=axisecp/acap-native-sdk:12.11.0-aarch64-ubuntu24.04
FROM ${SDK_IMAGE}
ARG ARCH=aarch64

WORKDIR /opt/app
COPY ./app .

# Bundle any models present in app/models (paths are preserved: models/detect.tflite).
RUN cp manifest.json.${ARCH} manifest.json && \
    . /opt/axis/acapsdk/environment-setup* && \
    if [ "$ARCH" = armv7hf ]; then export AURIX_LEGACY_SDK=1; fi && \
    EXTRA="" && \
    for f in models/*.tflite models/*.meta; do [ -f "$f" ] && EXTRA="$EXTRA -a $f"; done; \
    acap-build ./ $EXTRA
