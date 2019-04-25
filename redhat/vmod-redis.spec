Summary: Redis VMOD for Varnish
Name: vmod-redis
Version: 5.4
Release: 1%{?dist}
License: BSD
URL: https://github.com/carlosabalde/libvmod-redis
Group: System Environment/Daemons
Source0: libvmod-redis.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires: varnish >= 5.1.0, hiredis >= 0.11.0, libev >= 4.03
BuildRequires: make, python-docutils, varnish >= 5.1.0, varnish-devel >= 5.1.0, hiredis-devel >= 0.11.0, libev-devel >= 4.03

%description
Redis VMOD for Varnish

%prep
%setup -n libvmod-redis

%build
./autogen.sh
./configure --prefix=/usr/ --docdir='${datarootdir}/doc/%{name}' --libdir='%{_libdir}'
%{__make}
%{__make} check

%install
[ %{buildroot} != "/" ] && %{__rm} -rf %{buildroot}
%{__make} install DESTDIR=%{buildroot}

%clean
[ %{buildroot} != "/" ] && %{__rm} -rf %{buildroot}

%files
%defattr(-,root,root,-)
%{_libdir}/varnish*/vmods/lib*
%doc /usr/share/doc/%{name}/*
%{_mandir}/man?/*

%changelog
* Tue Aug 07 2018 Carlos Abalde <carlos.abalde@gmail.com> - 5.4-1.20180807
- Allowed disabling sickness TTL feature.
- Stopped WS-copying input arg in .command() & .push().
* Tue Jul 14 2018 Carlos Abalde <carlos.abalde@gmail.com> - 5.3-1.20180714
- Merged missing commit: support for CLUSTER NODES output in 4.x in tests.
* Thu May 03 2018 Carlos Abalde <carlos.abalde@gmail.com> - 5.2-1.20180503
- Fixed execution plan when clustering enabled.
* Thu Jun 01 2017 Carlos Abalde <carlos.abalde@gmail.com> - 5.1-1.20170601
- Added proxied methods.
* Fri Mar 17 2017 Carlos Abalde <carlos.abalde@gmail.com> - 5.0-1.20170317
- Migrated to Varnish Cache 5.1.x.
* Fri Mar 17 2017 Carlos Abalde <carlos.abalde@gmail.com> - 4.0-1.20170317
- Migrated to Varnish Cache 5.0.x.
* Wed Oct 26 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.11-1.20161026
- Added 'ignore_slaves' option to redis.db().
* Thu Aug 04 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.10-1.20160804
- Fixed Sentinel-related crash.
* Wed Aug 03 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.9-1.20160803
- Added full password support when clustering is enabled.
* Thu Jul 14 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.8-1.20160714
- Fixed crash when destroying ev loops.
* Wed Jul 13 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.7-1.20160713
- Fixed shared ev loop.
* Mon Jul 11 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.6-1.20160711
- Allowed server-less database instantiation.
* Thu Jun 09 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.5-1.20160609
- Avoided deadlocks enforcing lock ordering + simplifying locking model.
* Mon May 02 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.4-1.20160502
- Stop assuming down flag on +switch-master events.
* Wed Apr 20 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.3-1.20160420
- Added Redis Sentinel support.
- Added support for auto-discovery of roles.
- Replaced plain pthread mutexes by Varnish locks.
- Dynamic allocation of execution plans.
- Improved logging.
- Added redis.subnets() (previosuly redis.init()).
- Improvements & fixes.
* Tue Apr 12 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.2-1.20160412
- Improvements & fixes.
* Mon Apr 11 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.1-1.20160411
- Added support for smart command execution.
- Added redis.init(), including weights + subnet masks.
- Added type (master, slave, cluster) and sickness TTL to redis.db().
- Added master / slave selection to .execute().
- Improvements & fixes.
* Mon Apr 04 2016 Carlos Abalde <carlos.abalde@gmail.com> - 3.0-1.20160404
- New version numbering scheme.
* Mon Feb 29 2016 Carlos Abalde <carlos.abalde@gmail.com> - 0.3.6-1.20160229
- Improved VSL / syslog logging.
* Tue Feb 09 2016 Carlos Abalde <carlos.abalde@gmail.com> - 0.3.5-1.20160209
- Added AUTH support.
* Tue Dec 22 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.3.4-1.20151222
- Added .counter().
- Improved handling of -MOVED errors.
- Simplified & improved internals.
* Tue Dec 22 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.3.3-1.20151222
- Improvements & fixes.
* Mon Dec 21 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.3.2-1.20151221
- Added .stats().
* Wed Dec 16 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.3.1-1.20151216
- Removed support for server tags.
- Removed .add_cserver().
- Removed .server().
* Tue Dec 15 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.3.0-1.20151215
- Added support for multiple clusters.
- Added .retries().
- Removed redis.fini().
- Improved manual page.
* Mon Jun 08 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.2.7-1.20150608
- Fixed memory leak during redis.fini().
- Fixed initialization / reset of command execution timeout.
- Added Redis Cluster tests.
* Tue May 26 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.2.6-1.20150526
- Fixed bug when processing -MOVED and -ASK errors.
- Updated files borrowed from the Redis implementation.
* Tue May 12 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.2.5-1.20150512
- Do not require C99 standard.
* Fri Apr 17 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.2.4-1.20150417
- Added support for timeouts when executing commands.
- Maximum number of Redis Cluster hops is now configurable.
* Wed Jan 28 2015 Carlos Abalde <carlos.abalde@gmail.com> - 0.2.3-1.20150128
- Added support for hiredis 0.12.1 (redisEnableKeepAlive).
- Updated Redis Cluster key -> slot calculation.
* Fri Dec 19 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.2.2-1.20141219
- Added support for command retries.
* Wed Dec 17 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.2.1-1.20141217
- Added redis.replied().
- Minor fixes.
* Tue Dec 16 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.2.0-1.20141216
- Added Redis Cluster support.
- Minor improvements & fixes.
* Sun Dec 14 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.1.3-1.20141214
- Added support for shared pools of Redis connections.
- Refactor to simplify future support of Redis Cluster.
* Thu Oct 23 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.1.2-1.20141023
- Discard Redis contexts when connections are hung up by the server.
* Wed Sep 17 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.1.1-1.20140917
- Added missing WS_Dup()'s / WS_Copy()'s.
* Fri Aug 22 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.1-1.20140822
- Initial version.
