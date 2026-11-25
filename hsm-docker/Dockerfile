# 在 Docker 中编译并运行带 HSM 存储引擎的 PostgreSQL 15
#
#   docker build -t pg-hsm -f hsm-docker/Dockerfile .
#   docker run -it --rm --privileged pg-hsm
#
# --privileged 让容器可以用 losetup 创建 /dev/loopN 块设备；
# 不加也能运行，此时 HSM 直接使用一个普通文件作为“设备”。
FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential bison flex libreadline-dev zlib1g-dev \
      perl libipc-run-perl util-linux ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src
RUN ./configure --prefix=/usr/local/pgsql --enable-debug --enable-cassert \
        --enable-tap-tests CFLAGS="-O0 -g3" \
 && make -j"$(nproc)" -s \
 && make -s install \
 && make -s -C contrib/amcheck install \
 && make -s -C contrib/pageinspect install \
 && useradd -m postgres \
 && chown -R postgres /src

ENV PATH=/usr/local/pgsql/bin:$PATH
COPY hsm-docker/demo.sh /usr/local/bin/hsm-demo
COPY hsm-docker/demo.sql /usr/local/share/hsm-demo.sql
RUN chmod +x /usr/local/bin/hsm-demo
CMD ["hsm-demo"]
