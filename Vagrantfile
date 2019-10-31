# -*- mode: ruby -*-
# vi: set ft=ruby :

$script = <<SCRIPT
  # General packages.
  apt-get update -q
  apt-get install -qq unzip apt-transport-https \
    autotools-dev automake libtool python-docutils pkg-config libpcre3-dev \
    libeditline-dev libedit-dev make dpkg-dev git libjemalloc-dev \
    libev-dev libncurses-dev python-sphinx graphviz

  # Varnish Cache.
  sudo -u vagrant bash -c '\
    wget --no-check-certificate https://varnish-cache.org/_downloads/varnish-6.3.0.tgz; \
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
    wget --no-check-certificate https://github.com/redis/hiredis/archive/v0.14.0.zip -O hiredis-0.14.0.zip; \
    unzip hiredis-0.14.0.zip; \
    rm -f hiredis-0.14.0.zip; \
    cd hiredis*; \
    make; \
    sudo make PREFIX="/usr/local" install; \
    sudo ldconfig'

  # Redis.
  sudo -u vagrant bash -c '\
    cd /home/vagrant; \
    wget http://download.redis.io/releases/redis-5.0.5.tar.gz; \
    tar zxvf redis-*.tar.gz; \
    rm -f redis-*.tar.gz; \
    cd redis-*; \
    make; \
    sudo make PREFIX="/usr/local" install; \
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
  config.vm.synced_folder '.', '/vagrant', :nfs => false
  config.vm.provider :virtualbox do |vb|
    vb.memory = 1024
    vb.cpus = 1
    vb.linked_clone = Gem::Version.new(Vagrant::VERSION) >= Gem::Version.new('1.8.0')
    vb.customize [
      'modifyvm', :id,
      '--natdnshostresolver1', 'on',
      '--natdnsproxy1', 'on',
      '--accelerate3d', 'off',
      '--audio', 'none',
      '--paravirtprovider', 'Default',
    ]
  end

  config.vm.define :v63 do |machine|
    machine.vm.box = 'ubuntu/xenial64'
    machine.vm.box_version = '=20180315.0.0'
    machine.vm.box_check_update = true
    machine.vm.provision :shell, :privileged => true, :keep_color => false, :inline => $script
    machine.vm.provider :virtualbox do |vb|
      vb.customize [
        'modifyvm', :id,
        '--name', 'libvmod-redis (Varnish 6.3.x)',
      ]
    end
  end
end
