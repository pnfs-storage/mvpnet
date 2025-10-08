#!/bin/sh

# make sure to get the proxy information we already stored here...
. /etc/environment

distro=$(grep "^ID=" /etc/os-release|cut -f2 -d"="|tr -d '"')

case $distro in
  debian|ubuntu) DEBIAN_FRONTEND=noninteractive apt install -y -q $(cat /mnt/packagelist.${distro} | xargs) ;;
  fedora|rhel|centos|rocky|almalinux) DNF5_FORCE_INTERACTIVE=0 dnf -y install $(cat /mnt/packagelist.${distro} | xargs) ;;
  freebsd) pkg install -y $(cat /mnt/packagelist.${distro} | xargs) ;;
  *) echo "don't know how to install packages on this distribution..."
esac

# in environments with no access to a networked package repository, you can
# instead install packages from a local directory where you have stored them
# beforehand, for example:
# dpkg -i /mnt/packages/*.deb
# rpm -i /mnt/packages/*.rpm
