FROM ubuntu:noble-20240423

ENV DEBIAN_FRONTEND noninteractive

RUN groupadd -g 5000 dev \
    && useradd -u 5000 -g 5000 -m -s /bin/bash dev

RUN apt update \
    && apt install -y \
        apt-transport-https \
        automake \
        autotools-dev \
        bindfs \
        binutils \
        curl \
        dpkg-dev \
        git \
        gpg \
        graphviz \
        jq \
        less \
        libedit-dev \
        libev-dev \
        libjemalloc-dev \
        libncurses-dev \
        libpcre2-dev \
        libssl-dev \
        libtool \
        make \
        nano \
        netcat-traditional \
        pkg-config \
        python3 \
        python3-docutils \
        python3-sphinx \
        python3-venv \
        tar \
        telnet \
        unzip \
        wget \
    && apt clean \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/varnishcache/varnish-cache.git /tmp/varnish \
    && cd /tmp/varnish \
    && ./autogen.sh \
    && ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate https://github.com/redis/hiredis/archive/v1.2.0.zip -O hiredis-1.2.0.zip \
    && unzip hiredis-*.zip \
    && rm -f hiredis-*.zip \
    && cd hiredis* \
    && make USE_SSL=1 \
    && make USE_SSL=1 PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate http://download.redis.io/releases/redis-7.4.1.tar.gz \
    && tar zxvf redis-*.tar.gz \
    && rm -f redis-*.tar.gz \
    && cd redis-* \
    && make BUILD_TLS=yes \
    && make BUILD_TLS=yes PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate https://github.com/valkey-io/valkey/archive/refs/tags/8.0.1.tar.gz -O valkey-8.0.1.tar.gz \
    && tar zxvf valkey-*.tar.gz \
    && rm -f valkey-*.tar.gz \
    && cd valkey-* \
    && make BUILD_TLS=yes \
    && make BUILD_TLS=yes PREFIX='/usr/local' USE_REDIS_SYMLINKS=no install \
    && ldconfig

COPY ./docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
