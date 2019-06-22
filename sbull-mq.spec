#
# spec file for package sbull-mq
#
# Copyright (c) 2019 SUSE LINUX GmbH, Nuernberg, Germany.
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via http://bugs.opensuse.org/
#

#norootforbuild

Name:           sbull-mq
Version:        0.2
Release:        1
Summary:        Reimplementation of sbull, but now using blk-mq
License:        GPL-2.0-or-later
Group:          System/Kernel
Url:            https://github.com/marcosps/sbull-mq
Source0:        %{name}-%{version}.tar.gz
Source1:        %{name}-kmp-preamble
BuildRequires:  %kernel_module_package_buildreqs

%kernel_module_package -n %{name} -p %{name}-kmp-preamble

%description
This package contains the sbull-mq, which is the old sbull driver but now
converted to use blk-mq framework.

%prep
%setup
set -- *
mkdir source
mv "$@" source/
mkdir obj

%build
for flavor in %flavors_to_build; do
	rm -rf obj/$flavor
	cp -r source obj/$flavor
	make -C %{kernel_source $flavor} modules M=$PWD/obj/$flavor
done

%install
export INSTALL_MOD_PATH=$RPM_BUILD_ROOT
export INSTALL_MOD_DIR=updates
for flavor in %flavors_to_build; do
	make -C %{kernel_source $flavor} modules_install M=$PWD/obj/$flavor
done

%changelog
* Sat Jun 22 2019 Marcos Paulo de Souza <marcos.souza.org@gmail.com> - 0.2-0
- Fix build in kernel 4.12

* Fri Jun 21 2019 Marcos Paulo de Souza <marcos.souza.org@gmail.com> - 0.1-1
- Initial version
