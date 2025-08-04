### `cloud-init` configuration

Here is an example of how to use the `cloud-init` configuration system with mvpnet.

First, download a cloud-init image.  Here we use a CentOS Stream 9 image from:
https://cloud.centos.org/centos/9-stream/x86_64/images/CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2

Some preinstalled cloud disk images are constructed to be as small as possible,
leaving little room for the installation of additional software after the VM is
started.  These images can be resized with `qemu-img` before they are used with
mvpnet.  For example, to add an additional 3GB to the disk image:

```
$ qemu-img info CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2
image: CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2
file format: qcow2
virtual size: 10 GiB (10737418240 bytes)
disk size: 1.41 GiB
cluster_size: 65536
Format specific information:
    compat: 0.10
    compression type: zlib
    refcount bits: 16
Child node '/file':
    filename: CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2
    protocol type: file
    file length: 1.41 GiB (1517486080 bytes)
    disk size: 1.41 GiB
$ qemu-img resize CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2 +3G
Image resized.
$ qemu-img info CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2
image: CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2
file format: qcow2
virtual size: 13 GiB (13958643712 bytes)
disk size: 1.41 GiB
cluster_size: 65536
Format specific information:
    compat: 0.10
    compression type: zlib
    refcount bits: 16
Child node '/file':
    filename: CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2
    protocol type: file
    file length: 1.41 GiB (1517486592 bytes)
    disk size: 1.41 GiB
$
```
Note the change in the "virtual size" field.  The underlying filesystem should
be automatically resized by `cloud-init` to use all of the additional space
when the VM starts up.

Ensure that this image is available at the same path on all of the hosts that
will start mvpnet VMs.  This can be done by placing it in a network filesystem
common to all the hosts or by copying it to their local filesystems using `scp`,
`rsync`, `sftp`, etc.

Next, prepare a directory with the `cloud-init` configuration files.  This can
done with `./scripts/prep-config.sh` in this repository.  For example:
```
$ ./prep-config.sh ~/cloud-seed
INFO: no packagelist file specified, not adding any packages to install
INFO: no ssh key found, generating a new one for you

prep-config finished!
```

The resulting configuration is minimal, and you can add files to the cloud-seed
directory.  Here, we add two scripts, `imageprep.sh` and `mvpnet-init`:
```
cp scripts/imageprep.sh scripts/mvpnet-init ~/cloud-seed/
```
The first, `imageprep.sh`, does some one-time configuration and is invoked upon
VM instantiation.
The second, `mvpnet-init`, configures the special mvpnet network interface in
the VM and is invoked after every boot.

Finally, start up 4 VMs on two different hosts, `h0` and `h1`, with `mpirun.mpich`:
```
mpirun.mpich -n 4 -host h0,h1 -ppn 1         ./mvpnet/mvpnet -C 1 -c ~/cloud-seed         -j 13 -l /tmp/mvpnet/log         -M ./monwrap/monwrap -r /tmp/mvpnet/run -s /tmp/mvpnet/sock -t /tmp/mvpnet/tftpd -w 1         -g -L 1 -D INFO -S INFO         -n dgram foo -i /l0/images/CentOS-Stream-GenericCloud-x86_64-9-20250707.0.x86_64.qcow2,mvpctl=s
```

After successful startup, you can access the first VM via ssh on `h0`:
```
$ ssh -p2200 localhost
Warning: Permanently added '[localhost]:2200' (ED25519) to the list of known hosts.
Last login: Wed Jul 30 14:41:29 2025 from 192.168.1.2
[crd@n0000 ~]$ sudo -i
[root@n0000 ~]#
```

As this VM is rank 0 in the MPI job, its hostname is `n0000`.  From this shell,
the other VMs in this MPI job can be accessed over the mvpnet (10.0.0.0/8):
```
[root@n0000 ~]# cat /etc/hosts
127.0.0.1   localhost localhost.localdomain localhost4 localhost4.localdomain4
::1         localhost localhost.localdomain localhost6 localhost6.localdomain6

#wsize: 4
10.0.0.0 n0000
10.0.0.1 n0001
10.0.0.2 n0002
10.0.0.3 n0003
[root@n0000 ~]# ssh n0003
Warning: Permanently added 'n0003' (ED25519) to the list of known hosts.
Activate the web console with: systemctl enable --now cockpit.socket

[root@n0003 ~]# df -h /
Filesystem      Size  Used Avail Use% Mounted on
/dev/vda4       8.8G  1.1G  7.7G  13% /
[root@n0003 ~]#
```

