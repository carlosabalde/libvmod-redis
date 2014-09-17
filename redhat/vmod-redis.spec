Summary: Redis VMOD for Varnish
Name: vmod-redis
Version: 0.1.1
Release: 1%{?dist}
License: BSD
URL: https://github.com/carlosabalde/libvmod-redis
Group: System Environment/Daemons
Source0: libvmod-redis.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires: varnish > 3.0, hiredis >= 0.11.0
BuildRequires: make, python-docutils

%description
Redis VMOD for Varnish

%prep
%setup -n libvmod-redis

%build
./configure VMODDIR=%{VMODDIR} --prefix=/usr/ --docdir='${datarootdir}/doc/%{name}'
make
make check

%install
make install DESTDIR=%{buildroot}

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
%{_libdir}/varnish*/vmods/
%doc /usr/share/doc/%{name}/*
%{_mandir}/man?/*

%changelog
* Wed Sep 17 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.1.1-0.20140917
- Added missing WS_Dup()'s / WS_Copy()'s.
* Sun Aug 22 2014 Carlos Abalde <carlos.abalde@gmail.com> - 0.1-0.20140822
- Initial version.
