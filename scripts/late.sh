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
cp /mnt/id_ed25519 /mnt/id_ed25519.pub /root/.ssh/
chmod 400 /root/.ssh/id_ed25519

# XXXCRD: change crd to whatever was selected as the c-i default user
# for normal user
cp /mnt/id_ed25519 /mnt/id_ed25519.pub /home/crd/.ssh/
chown crd:crd /home/crd/.ssh/id_ed25519 /home/crd/.ssh/id_ed25519.pub
chmod 400 /home/crd/.ssh/id_ed25519
