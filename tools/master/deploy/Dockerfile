# The TM2 PC master server in a container (a Synology NAS's Container Manager, or any Docker host): built from the
# master's own files in this folder, run as an unprivileged user on UDP 27522.
FROM alpine:3.20 AS build
RUN apk add --no-cache gcc musl-dev
WORKDIR /src
COPY master.c master.h accounts.c accounts.h net_udp.c net_udp.h session.h rollback.h ./
RUN gcc -O2 -I. -o tm2_master master.c accounts.c net_udp.c

FROM alpine:3.20
COPY --from=build /src/tm2_master /usr/local/bin/tm2_master
# the players, friends and stats (tools/master/accounts.c): a volume, kept when the container is built again
RUN mkdir -p /data && chown nobody /data
VOLUME /data
USER nobody
CMD ["/usr/local/bin/tm2_master", "27522", "/data/tm2_master.db"]
