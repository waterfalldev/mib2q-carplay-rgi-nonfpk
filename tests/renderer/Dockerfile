# qnx65-armv7-toolchain plus host g++ (the base image has gcc only).
FROM qnx65-armv7-toolchain:latest
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
