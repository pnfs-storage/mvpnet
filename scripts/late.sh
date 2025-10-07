#!/bin/sh
# late  a one-time sequence of tasks to perform on a disk image
# late during initial VM instantiation
#
# for tasks that can/should happen early during cloud-init instantiation,
# see imageprep.sh
#
# 2025-07-28  crd@andrew.cmu.edu
#

# install ssh keys for MPI usage
#  for root
cp /mnt/id_ed25519 /mnt/id_ed25519.pub ~root/.ssh/
cat /mnt/id_ed25519.pub >> ~root/.ssh/authorized_keys
chmod 400 ~root/.ssh/id_ed25519

# XXXCRD: there doesn't seem to be a better way of figuring out the c-i default
#  username(s) than grepping them out of the logs.  it might not be too robust...
# /var/log/cloud-init.log:2025-08-26 17:45:20,767 - distros[DEBUG]: Adding user crd
for u in $(grep ": Adding user " /var/log/cloud-init*|awk '{print $NF}'); do
  mkdir -p -m700 /home/${u}/.ssh
  cp /mnt/id_ed25519 /mnt/id_ed25519.pub /home/${u}/.ssh/
  chown ${u}:${u} /home/${u}/.ssh/id_ed25519 /home/${u}/.ssh/id_ed25519.pub
  chmod 400 /home/${u}/.ssh/id_ed25519
done
