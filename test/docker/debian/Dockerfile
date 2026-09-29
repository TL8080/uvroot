FROM debian:13

COPY . /usr/src/uvroot

RUN useradd -m -u 1000 uvroot && \
    chown -R uvroot:uvroot /usr/src/uvroot

WORKDIR /usr/src/uvroot
USER uvroot
