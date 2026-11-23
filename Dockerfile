FROM alpine:3.22 AS build

RUN apk add --no-cache \
    build-base \
    pkgconf \
    curl-dev \
    nlohmann-json

WORKDIR /build

COPY youtube-monitor.cpp monitor-service.inc visionos-client.inc ./

RUN g++ -std=c++17 -O2 -Wall -Wextra -pthread \
    youtube-monitor.cpp -o youtube-monitor \
    $(pkg-config --cflags --libs libcurl)

FROM alpine:3.22

RUN apk add --no-cache \
    ca-certificates \
    libcurl \
    libstdc++

WORKDIR /app

COPY --from=build /build/youtube-monitor /app/youtube-monitor

USER 10001

ENV YTM_INTERVAL=3600

EXPOSE 8000

CMD ["/app/youtube-monitor"]
