# -*- mode: ruby -*-
# vi: set ft=ruby :

$script = <<SCRIPT
  sudo apt-get update -q
  sudo apt-get install -qq libhiredis-dev redis-server apt-transport-https \
    autotools-dev automake libtool python-docutils pkg-config libpcre3-dev \
    libeditline-dev libedit-dev make dpkg-dev

  curl https://repo.varnish-cache.org/debian/GPG-key.txt | sudo apt-key add -
  echo "deb https://repo.varnish-cache.org/ubuntu/ trusty varnish-3.0" | sudo tee /etc/apt/sources.list.d/varnish-cache.list
  echo "deb-src https://repo.varnish-cache.org/ubuntu/ trusty varnish-3.0" | sudo tee -a /etc/apt/sources.list.d/varnish-cache.list
  sudo apt-get update -q
  sudo apt-get install -qq varnish libvarnishapi-dev

  sudo -u vagrant bash -c '\
    cd /home/vagrant; \
    wget --no-check-certificate https://repo.varnish-cache.org/ubuntu/pool/varnish-3.0/v/varnish/varnish_3.0.6.orig.tar.gz; \
    tar zxvf varnish_3.0.6.orig.tar.gz; \
    rm -f varnish_3.0.6.orig.tar.gz; \
    cd varnish*; \
    ./configure; \
    make'

  sudo -u vagrant bash -c '\
    cd /vagrant; \
    ./autogen.sh; \
    ./configure VARNISHSRC=/home/vagrant/varnish* VMODDIR=`/usr/lib/varnish/vmods/`; \
    make'
SCRIPT

Vagrant.configure('2') do |config|
  config.vm.hostname = 'dev'
  config.vm.network :public_network
  config.vm.synced_folder '.', '/vagrant', :nfs => false
  config.vm.provider :virtualbox do |vb|
    vb.customize [
      'modifyvm', :id,
      '--memory', '1024',
      '--natdnshostresolver1', 'on',
      '--accelerate3d', 'off',
    ]
  end

  config.vm.define :v3 do |machine|
    machine.vm.box = 'ubuntu/trusty64'
    machine.vm.box_version = '=14.04'
    machine.vm.box_check_update = true
    machine.vm.provision :shell, :inline => $script
    machine.vm.provider :virtualbox do |vb|
      vb.customize [
        'modifyvm', :id,
        '--name', 'libvmod-redis (Varnish 3.x)',
      ]
    end
  end
end
