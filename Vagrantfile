# -*- mode: ruby -*-
# vi: set ft=ruby :

$script = <<SCRIPT
  # Configuration.
  REDIS_PORTS="6380 6381 6382 6390 6391 6392 6400 6401 6402"
  REDIS_CLUSTER_PORTS="7000 7001 7002 7003 7004 7005 7006 7007 7008"

  # General packages.
  apt-get update -q
  apt-get install -qq unzip apt-transport-https \
    autotools-dev automake libtool python-docutils pkg-config libpcre3-dev \
    libeditline-dev libedit-dev make dpkg-dev git libjemalloc-dev \
    libev-dev libncurses-dev python-sphinx graphviz ruby
  gem install redis -v 3.3.3

  # Varnish Cache.
  sudo -u vagrant bash -c '\
    git clone https://github.com/varnishcache/varnish-cache.git /tmp/varnish; \
    cd /tmp/varnish; \
    ./autogen.sh; \
    ./configure; \
    make; \
    sudo make PREFIX="/usr/local" install; \
    sudo ldconfig'

  # hiredis.
  sudo -u vagrant bash -c '\
    cd /home/vagrant; \
    wget --no-check-certificate https://github.com/redis/hiredis/archive/v0.13.3.zip -O hiredis-0.13.3.zip; \
    unzip hiredis-0.13.3.zip; \
    rm -f hiredis-0.13.3.zip; \
    cd hiredis*; \
    make; \
    sudo make PREFIX="/usr/local" install; \
    sudo ldconfig'

  # Redis.
  sudo -u vagrant bash -c '\
    cd /home/vagrant; \
    wget http://download.redis.io/releases/redis-4.0.1.tar.gz; \
    tar zxvf redis-*.tar.gz; \
    rm -f redis-*.tar.gz; \
    cd redis-*; \
    make; \
    sudo make PREFIX="/usr/local" install; \
    sudo ldconfig; \
    sudo cp src/redis-trib.rb /usr/local/bin'

  # General Redis setup.
  mkdir -p /etc/redis /var/lib/redis
  for PORT in $REDIS_PORTS $REDIS_CLUSTER_PORTS; do
    cp /home/vagrant/redis*/utils/redis_init_script /etc/init.d/redis-server-$PORT
    sed /etc/init.d/redis-server-$PORT -i \
      -e "s%^REDISPORT=.*%REDISPORT=$PORT%" \
      -e "s%^PIDFILE=/var/run/redis_%PIDFILE=/var/run/redis-%"
    chmod +x /etc/init.d/redis-server-$PORT
    update-rc.d -f redis-server-$PORT defaults

    cp /home/vagrant/redis*/redis.conf /etc/redis/$PORT.conf
    sed /etc/redis/$PORT.conf -i \
      -e "s%^port .*%port $PORT%" \
      -e "s%^dir .*%dir /var/lib/redis%" \
      -e "s%^daemonize .*%daemonize yes%" \
      -e "s%^pidfile .*%pidfile /var/run/redis-$PORT.pid%" \
      -e "s%^# unixsocket .*%unixsocket /tmp/redis-$PORT.sock%" \
      -e "s%^# unixsocketperm .*%unixsocketperm 777%" \
      -e "s%^dbfilename .*%dbfilename dump-$PORT.rdb%" \
      -e "s%^appendfilename .*%appendfilename appendonly-$PORT.aod%"
  done

  # Classic Redis setup.
  for PORT in $REDIS_PORTS; do
    if [ `expr $PORT % 10` -ne "0" ]; then
      MASTER_PORT=`expr $PORT - $PORT % 10`
      sed /etc/redis/$PORT.conf -i \
        -e "s%^logfile .*%logfile /var/log/redis-$PORT.log%" \
        -e "s%^# slaveof .*%slaveof 127.0.0.1 $MASTER_PORT%"
    fi

    service redis-server-$PORT start
  done

  # Redis Cluster setup.
  REDIS_CLUSTER_NODES=""
  for PORT in $REDIS_CLUSTER_PORTS; do
    REDIS_CLUSTER_NODES="$REDIS_CLUSTER_NODES 127.0.0.1:$PORT"

    sed /etc/redis/$PORT.conf -i \
      -e "s%^logfile .*%logfile /var/log/redis-$PORT.log%" \
      -e "s%^# cluster-enabled .*%cluster-enabled yes%" \
      -e "s%^# cluster-config-file .*%cluster-config-file nodes-$PORT.conf%" \
      -e "s%^# cluster-node-timeout .*%cluster-node-timeout 5000%" \
      -e "s%^appendonly .*%appendonly yes%"

    service redis-server-$PORT start
  done

  sudo -u vagrant bash -c "\
    echo '/home/vagrant/redis*/src/redis-trib.rb create --replicas 2 $REDIS_CLUSTER_NODES' \
      > /home/vagrant/create-redis-cluster.sh; \
    chmod +x /home/vagrant/create-redis-cluster.sh"

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

  config.vm.define :master do |machine|
    machine.vm.box = 'ubuntu/xenial64'
    machine.vm.box_version = '=20180315.0.0'
    machine.vm.box_check_update = true
    machine.vm.provision :shell, :privileged => true, :keep_color => false, :inline => $script
    machine.vm.provider :virtualbox do |vb|
      vb.customize [
        'modifyvm', :id,
        '--name', 'libvmod-redis (Varnish master)',
      ]
    end
  end
end
