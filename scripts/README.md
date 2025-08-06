# Supplemental `mvpnet` scripts

This directory contains scripts that are useful with `mvpnet`:

* `mvpnet.sh` and `mvpnet.pl`: the mvp network interface is special and is not
configured normally by the qemu dhcp server.  These scripts configure that
interface and some other things based on the configuration of the mvpnet MPI
world.  The shell version is provided since some distribution's cloud images no
longer include perl.

* `imageprep.sh`: a one-time sequence of tasks to perform before or during
initial VM instantiation, for example in `cloud-init-per once`:
    ```
    bootcmd:
      - [ cloud-init-per, once, imageprep, sh, -c, '/mnt/imageprep.sh' ]
    ```
    It is more convenient to perform tasks this way than to enumerate them
    individually in the `cloud-init` configuration. This script could also be used
    to manually prepare a disk image without `cloud-init`.

    While this script is usable as-is, it is also meant to serve as an example of
    how images can be prepared for use in the mvpnet environment.

* `prep-config.sh`: prepares data for use in the `cloud-init` configuration
environment.  This script is also meant to serve as an example of what can be
performed with this configuration method.

