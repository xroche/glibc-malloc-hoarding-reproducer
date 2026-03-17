FROM ubuntu:22.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY reproducer.c Makefile ./
RUN make

ENTRYPOINT ["./reproducer"]
CMD ["--threads", "16", "--index-mb", "16", "--burst-mb", "64", "--bursts", "10", "--trim", "--verbose"]
