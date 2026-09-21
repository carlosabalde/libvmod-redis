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

RUN git clone --recurse-submodules https://github.com/varnish/varnish.git /tmp/varnish \
    && cd /tmp/varnish \
    && git checkout varnish-9.1.0 \
    && ./autogen.sh \
    && CC="${VCC}" ./configure --prefix=/opt/varnish \
    && make \
    && make install \
    && echo /opt/varnish/lib > /etc/ld.so.conf.d/varnish.conf \
    && ldconfig

RUN cd /tmp \
    && wget https://vinyl-cache.org/downloads/vinyl-cache-9.1.0.tgz \
    && tar zxvf vinyl-cache-9.1.0.tgz \
    && rm -f vinyl-cache-9.1.0.tgz \
    && cd vinyl-cache-9.1.0 \
    && ./autogen.sh \
    && CC="${VCC}" ./configure --prefix=/opt/vinyl-cache \
    && make \
    && make install \
    && echo /opt/vinyl-cache/lib > /etc/ld.so.conf.d/vinyl-cache.conf \
    && ldconfig

# Varnish & Vinyl are installed under separate '/opt' prefixes because both ship
# a 'vtest' hard link (varnishtest / vinyltest) that would otherwise collide in
# '/usr/local/bin', with the winner silently decided by install order. Separate
# prefixes also guarantee the vtest core and the -E extension always come from
# the same project, even if their vtest2 submodules diverge. Exposing both
# pkgconfig dirs is unambiguous since the module names differ (varnishapi vs.
# vinylapi): './configure --with-vcache=<flavor>' picks one, and every other
# path (PATH for tests, LD_LIBRARY_PATH, VTESTEXT, vmoddir) is derived from it.
ENV PKG_CONFIG_PATH=/opt/varnish/lib/pkgconfig:/opt/vinyl-cache/lib/pkgconfig

RUN cd /tmp \
    && wget https://github.com/redis/hiredis/archive/v1.4.1.zip -O hiredis-1.4.1.zip \
    && unzip hiredis-*.zip \
    && rm -f hiredis-*.zip \
    && cd hiredis* \
    && make USE_SSL=1 \
    && make USE_SSL=1 PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget https://github.com/redis/redis/archive/refs/tags/8.10.1.tar.gz -O redis-8.10.1.tar.gz \
    && tar zxvf redis-*.tar.gz \
    && rm -f redis-*.tar.gz \
    && cd redis-* \
    && make BUILD_TLS=yes \
    && make BUILD_TLS=yes PREFIX='/usr/local' install \
    && ldconfig

COPY ./docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
