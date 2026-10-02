# Usage: docker build --build-arg SDK_IMAGE=axisecp/acap-native-sdk:<tag> -t aurix .
# The SDK tag decides the target architecture and the minimum AXIS OS version.
ARG SDK_IMAGE=axisecp/acap-native-sdk:latest-aarch64
FROM ${SDK_IMAGE}

WORKDIR /opt/app
COPY ./app .

# Bundle any models present in app/models; build still succeeds without them.
RUN . /opt/axis/acapsdk/environment-setup* && \
    EXTRA="" && \
    for f in models/*.tflite; do [ -f "$f" ] && EXTRA="$EXTRA -a $f"; done; \
    acap-build ./ $EXTRA
