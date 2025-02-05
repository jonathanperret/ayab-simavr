# Build stage
FROM debian:bullseye-slim AS build

# Install required dependencies
RUN apt-get update -qq && apt-get install -y -qq \
    build-essential \
    git \
    avr-libc \
    gcc-avr \
    pkg-config \
    libelf-dev \
    make \
    gcc \
    && rm -rf /var/lib/apt/lists/*

# Create working directory
WORKDIR /src

# Clone simavr repository
COPY . /src

# Patch Makefile.common to call pkg-config with the --static flag
RUN sed -i 's/pkg-config --libs/pkg-config --static --libs/g' Makefile.common

# Build simavr
RUN make -C simavr -j$(nproc)

RUN find . -name 'libsimavr.so*' -delete

# Build ayab
RUN make -C examples/board_ayab -j$(nproc)

# Runtime stage
FROM scratch

# Copy the built binary from the build stage
COPY --from=build /src/examples/board_ayab/obj-*/ayab.elf /ayab

ENTRYPOINT ["/ayab"]