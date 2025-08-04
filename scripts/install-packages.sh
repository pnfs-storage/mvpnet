#!/bin/sh

# examples of how to install packages.  ok if some fail...
DEBIAN_FRONTEND=noninteractive apt install -y -q $(</mnt/packagelist)

DNF5_FORCE_INTERACTIVE=0 dnf install $(</mnt/packagelist)
