FROM ubuntu:noble-20250714

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

RUN cd /tmp \
    && wget --no-check-certificate https://varnish-cache.org/_downloads/varnish-7.7.0.tgz \
    && tar zxvf varnish-*.tgz \
    && rm -f varnish-*.tgz \
    && cd varnish-* \
    && ./autogen.sh \
    && ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate https://github.com/redis/hiredis/archive/v1.3.0.zip -O hiredis-1.3.0.zip \
    && unzip hiredis-*.zip \
    && rm -f hiredis-*.zip \
    && cd hiredis* \
    && make USE_SSL=1 \
    && make USE_SSL=1 PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate https://github.com/redis/redis/archive/refs/tags/8.0.0.tar.gz -O redis-8.0.0.tar.gz \
    && tar zxvf redis-*.tar.gz \
    && rm -f redis-*.tar.gz \
    && cd redis-* \
    && make BUILD_TLS=yes \
    && make BUILD_TLS=yes PREFIX='/usr/local' install \
    && ldconfig

COPY ./docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
