# Supplemental `mvpnet` scripts

This directory contains scripts that are useful with `mvpnet`:

* `imageprep.sh`: a one-time sequence of tasks to perform before or during
initial VM instantiation, for example in `cloud-init-per once`:
    ```
    bootcmd:
      - [ cloud-init-per, once, imageprep, sh, -c, '/mnt/imageprep.sh' ]
    ```
    (The `prep-config.sh` script described below takes care of this for you if
    you are using this method.)

    It is more convenient to perform tasks this way than to enumerate them
    individually in the `cloud-init` configuration. This script could also be used
    to manually prepare a disk image without `cloud-init`.

    While this script is usable as-is, it is also meant to serve as an example of
    how images can be prepared for use in the mvpnet environment.

* `mvpnet.sh` and `mvpnet.pl`: the mvp network interface is special and is not
configured normally by the qemu dhcp server.  These scripts configure that
interface and some other things based on the configuration of the mvpnet MPI
world.  The shell version is provided since some distribution's cloud images no
longer include perl.  If using the cloud-init method of image preparation, it is
best to copy one of these two scripts into your cloud-seed directory as
`mvpnet-init` and then add it as a cloud-init `bootcmd`, for example:
    ```
    bootcmd:
      - [ cloud-init-per, always, mvpnet-init, sh, -c, '/mnt/mvpnet-init' ]
    ```
    (The `prep-config.sh` script described below takes care of this for you if
    you are using this method.)

* `prep-config.sh`: prepares data for use in the `cloud-init` configuration
environment.  This script is also meant to serve as an example of what can be
performed with this configuration method.

* `proxy-config.sh`: if your environment requires the use of a proxy server,
configure it using this script

* `install-packages.sh`: shows how one might go about automatically adding a
set of software packages at the time guest VM is instantiated.  It gets its
input from a plain text file in the cloud-seed directory named
`packagelist.${distro}` containing a list of package names, one per line.  It
should work for any distro of
`debian|ubuntu|fedora|rhel|centos|rocky|almalinux|freebsd`.  You could also
choose to replace the entire file with a customized installation script.

