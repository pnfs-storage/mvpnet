#!/bin/sh
# imageprep  a one-time sequence of tasks to perform on a disk image
# before or very early during initial VM instantiation
#
# for tasks that can/should happen later during cloud-init instantiation,
# see scripts/late.sh
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

# add uncommented line (otherwise mpich fails):
echo '    StrictHostKeyChecking no' >> /etc/ssh/ssh_config

# freebsd doesn't allow root logins by default
echo 'PermitRootLogin prohibit-password' >> /etc/ssh/sshd_config

# remove /etc/hostname so that guest gets hostname from qemu dhcp:
rm -f /etc/hostname
