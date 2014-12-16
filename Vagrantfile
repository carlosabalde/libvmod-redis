# -*- mode: ruby -*-
# vi: set ft=ruby :

$script = <<SCRIPT
  # Configuration.
  REDIS_PORTS="6380 6381 6382 6390 6391 6392 6400 6401 6402"
  REDIS_CLUSTER_PORTS="7000 7001 7002 7003 7004 7005 7006 7007 7008"

  # General packages.
  apt-get update -q
  apt-get install -qq libhiredis-dev apt-transport-https \
    autotools-dev automake libtool python-docutils pkg-config libpcre3-dev \
    libeditline-dev libedit-dev make dpkg-dev
  gem install redis

  # Varnish Cache.
  curl https://repo.varnish-cache.org/debian/GPG-key.txt | apt-key add -
  echo "deb https://repo.varnish-cache.org/ubuntu/ trusty varnish-4.0" > /etc/apt/sources.list.d/varnish-cache.list
  apt-get update -q
  apt-get install -qq varnish libvarnishapi-dev
  sudo cp /usr/local/share/aclocal/varnish.m4 /usr/share/aclocal/

  # Redis.
  sudo -u vagrant bash -c '\
    cd /home/vagrant; \
    wget https://github.com/antirez/redis/archive/3.0.0-rc1.tar.gz; \
    tar zxvf 3.0.0-rc1.tar.gz; \
    rm -f 3.0.0-rc1.tar.gz; \
    cd redis*; \
    make; \
    sudo make PREFIX="/usr/local" install; \
    sudo ldconfig'

  # General Redis setup.
  mkdir -p /etc/redis /var/lib/redis
  for PORT in $REDIS_PORTS $REDIS_CLUSTER_PORTS; do
    cp /home/vagrant/redis*/utils/redis_init_script /etc/init.d/redis-server-$PORT
    sed /etc/init.d/redis-server-$PORT -i \
      -e "s%^REDISPORT=.*%REDISPORT=$PORT%" \
      -e "s%^PIDFILE=/var/run/redis_%PIDFILE=/var/run/redis-%" \
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
        -e "s%^# slaveof .*%slaveof 127.0.0.1 $MASTER_PORT%"
    fi

    service redis-server-$PORT start
  done

  # Redis Cluster setup.
  REDIS_CLUSTER_NODES=""
  for PORT in $REDIS_CLUSTER_PORTS; do
    REDIS_CLUSTER_NODES="$REDIS_CLUSTER_NODES 127.0.0.1:$PORT"

    sed /etc/redis/$PORT.conf -i \
      -e "s%^# cluster-enabled .*%cluster-enabled yes%" \
      -e "s%^# cluster-config-file .*%cluster-config-file nodes-$PORT.conf%"

    service redis-server-$PORT start
  done
  echo "/home/vagrant/redis*/src/redis-trib.rb create --replicas 2 $REDIS_CLUSTER_NODES" \
    > /home/vagrant/create-redis-cluster.sh

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
    vb.customize [
      'modifyvm', :id,
      '--memory', '1024',
      '--natdnshostresolver1', 'on',
      '--accelerate3d', 'off',
    ]
  end

  config.vm.define :v4 do |machine|
    machine.vm.box = 'ubuntu/trusty64'
    machine.vm.box_version = '=14.04'
    machine.vm.box_check_update = true
    machine.vm.provision :shell, :privileged => true, :keep_color => false, :inline => $script
    machine.vm.provider :virtualbox do |vb|
      vb.customize [
        'modifyvm', :id,
        '--name', 'libvmod-redis (Varnish 4.x)',
      ]
    end
  end
end
