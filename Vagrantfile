# -*- mode: ruby -*-
# vi: set ft=ruby :

$script = <<SCRIPT
  # General packages.
  apt-get update -q
  apt-get install -qq unzip apt-transport-https \
    autotools-dev automake libtool python3-docutils pkg-config libpcre2-dev \
    libedit-dev make dpkg-dev git libjemalloc-dev \
    libev-dev libncurses-dev python3-sphinx graphviz libssl-dev

  # Varnish Cache.
  sudo -u vagrant bash -c '\
    wget --no-check-certificate https://varnish-cache.org/_downloads/varnish-7.2.0.tgz; \
    tar zxvf varnish-*.tgz; \
    rm -f varnish-*.tgz; \
    cd varnish-*; \
    ./autogen.sh; \
    ./configure; \
    make; \
    sudo make PREFIX="/usr/local" install; \
    sudo ldconfig'

  # hiredis.
  sudo -u vagrant bash -c '\
    cd /home/vagrant; \
    wget --no-check-certificate https://github.com/redis/hiredis/archive/v1.0.2.zip -O hiredis-1.0.2.zip; \
    unzip hiredis-*.zip; \
    rm -f hiredis-*.zip; \
    cd hiredis*; \
    make USE_SSL=1; \
    sudo make USE_SSL=1 PREFIX="/usr/local" install; \
    sudo ldconfig'

  # Redis.
  sudo -u vagrant bash -c '\
    cd /home/vagrant; \
    wget http://download.redis.io/releases/redis-7.0.9.tar.gz; \
    tar zxvf redis-*.tar.gz; \
    rm -f redis-*.tar.gz; \
    cd redis-*; \
    make BUILD_TLS=yes; \
    sudo make BUILD_TLS=yes PREFIX="/usr/local" install; \
    sudo ldconfig'

  # VMOD.
  sudo -u vagrant bash -c '\
    cd /vagrant; \
    ./autogen.sh; \
    ./configure; \
    make'
SCRIPT

Vagrant.configure('2') do |config|
  config.vm.hostname = 'dev'
  config.vm.network :public_network
  config.vm.synced_folder '.', '/vagrant', type: 'virtualbox'
  config.vm.provider :virtualbox do |vb|
    vb.memory = 1024
    vb.cpus = 1
    vb.linked_clone = true
    vb.customize [
      'modifyvm', :id,
      '--natdnshostresolver1', 'on',
      '--natdnsproxy1', 'on',
      '--accelerate3d', 'off',
      '--audio', 'none',
      '--paravirtprovider', 'Default',
    ]
  end

  config.vm.define :v72 do |machine|
    machine.vm.box = 'ubuntu/jammy64'
    machine.vm.box_version = '=20221018.0.0 '
    machine.vm.box_check_update = true
    machine.vm.provision :shell, :privileged => true, :keep_color => false, :inline => $script
    machine.vm.provider :virtualbox do |vb|
      vb.customize [
        'modifyvm', :id,
        '--name', 'libvmod-redis (Varnish 7.2.x)',
      ]
    end
  end
end
