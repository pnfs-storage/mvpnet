#!/bin/sh

# make sure to get the proxy information we already stored here...
. /etc/environment

# examples of how to install packages.  ok if some fail...
DEBIAN_FRONTEND=noninteractive apt install -y -q $(</mnt/packagelist)

DNF5_FORCE_INTERACTIVE=0 dnf -y install $(</mnt/packagelist)

# in environments with no access to a networked package repository, you can
# instead install packages from a local directory where you have stored them
# beforehand, for example:
# dpkg -i /mnt/packages/*.deb
# rpm -i /mnt/packages/*.rpm
