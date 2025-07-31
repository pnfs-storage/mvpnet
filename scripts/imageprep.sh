#!/bin/sh
# imageprep  a one-time sequence of task's to perform on a disk image
# before or during initial VM instantiation
#
# 2025-07-28  crd@andrew.cmu.edu
#

# proxy config
# create this script if you need to use a proxy in your network environment
/mnt/proxy-config.sh

# install ssh keys for MPI usage
#  for root
cp /mnt/id_ed25519 /mnt/id_ed25519.pub /root/.ssh/
chmod 400 /root/.ssh/id_ed25519
cat /mnt/id_ed25519.pub >> /root/.ssh/authorized_keys
# for normal user
# XXX: how to get default username???

# add uncommented line (otherwise mpich fails):
echo '    StrictHostKeyChecking no' >> /etc/ssh/ssh_config

# remove /etc/hostname so that guest gets hostname from qemu dhcp:
rm -f /etc/hostname
