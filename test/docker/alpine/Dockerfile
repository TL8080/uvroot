FROM alpine:latest

COPY . /usr/src/uvroot

RUN adduser -D -u 1000 uvroot && \
    chown -R uvroot:uvroot /usr/src/uvroot

WORKDIR /usr/src/uvroot
USER uvroot
