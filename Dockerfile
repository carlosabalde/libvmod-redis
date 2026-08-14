FROM ubuntu:resolute-20260421

ARG VCC=gcc

ENV DEBIAN_FRONTEND=noninteractive

RUN groupadd -g 5000 dev \
    && useradd -u 5000 -g 5000 -m -s /bin/bash dev

RUN apt update \
    && apt install -y \
        apt-transport-https \
        automake \
        autoconf-archive \
        autotools-dev \
        bindfs \
        binutils \
        bsdextrautils \
        clang \
        cpio \
        curl \
        dpkg-dev \
        furo \
        git \
        gpg \
        graphviz \
        jq \
        lcov \
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
    && wget --no-check-certificate https://github.com/varnish/varnish/releases/download/varnish-9.0.0/varnish-9.0.0.tar.gz \
    && tar zxvf varnish-*.tar.gz \
    && rm -f varnish-*.tar.gz \
    && cd varnish-* \
    && ./autogen.sh \
    && CC="${VCC}" ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate https://vinyl-cache.org/downloads/vinyl-cache-9.0.1.tgz \
    && tar zxvf vinyl-cache-9.0.1.tgz \
    && rm -f vinyl-cache-9.0.1.tgz \
    && cd vinyl-cache-9.0.1 \
    && ./autogen.sh \
    && CC="${VCC}" ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate https://github.com/redis/hiredis/archive/v1.4.1.zip -O hiredis-1.4.1.zip \
    && unzip hiredis-*.zip \
    && rm -f hiredis-*.zip \
    && cd hiredis* \
    && make USE_SSL=1 \
    && make USE_SSL=1 PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate https://github.com/redis/redis/archive/refs/tags/8.10.0.tar.gz -O redis-8.10.0.tar.gz \
    && tar zxvf redis-*.tar.gz \
    && rm -f redis-*.tar.gz \
    && cd redis-* \
    && make BUILD_TLS=yes \
    && make BUILD_TLS=yes PREFIX='/usr/local' install \
    && ldconfig

COPY ./docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
