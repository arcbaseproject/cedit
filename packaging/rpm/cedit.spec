Name:           cedit
Version:        1.0.1
Release:        1%{?dist}
Summary:        Small terminal text editor with nano keys and language-server support
License:        GPL-3.0-or-later
URL:            https://github.com/arcbaseproject/cedit
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make

%description
Cedit is a terminal text editor written in C. It uses nano keys by
default and offers Emacs and Ctrl+S/Ctrl+Q key layouts. It handles large
files, saves atomically, highlights eleven languages and talks to
language servers for diagnostics and completion.

%prep
%autosetup

%build
%make_build

%check
%set_build_flags
make test

%install
%make_install PREFIX=%{_prefix} MANDIR=%{_mandir} DOCDIR=%{_pkgdocdir}

%files
%license LICENSE
%{_bindir}/cedit
%{_mandir}/man1/cedit.1*
%{_pkgdocdir}/

%changelog
* Sat Nov 14 2026 arcbaseproject - 1.0.1-1
- Update to 1.0.1

* Sat Nov 14 2026 arcbaseproject - 1.0.0-1
- First package
