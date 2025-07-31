# mvpnet

The mvpnet program is an MPI application that allows users to launch
a set of [qemu-based](https://www.qemu.org/) virtual machines (VMs)
as an MPI job.   Users are free to choose the guest operating systems
to run and have full root access to the guest.  Each mvpnet MPI rank
runs its own guest VM under qemu.  Guest operating systems communicate
with each other using a MPI-based virtual private network managed
by mvpnet.

Each mvpnet guest VM has a virtual Ethernet interface configured
using qemu's `-netdev stream` or `-netdev dgram` flags.  The qemu
program connects this type of virtual Ethernet interface to a
unix domain socket file on the host system.   The mvpnet application
reads Ethernet frames sent by its guest OS from its Ethernet interface
socket file.  It then uses MPI point-to-point operations to forward
the frame to the mvpnet rank running the destination guest VM.
The destination mvpnet rank delivers the Ethernet frame to its
guest VM by writing it to the VM's socket file.

In order to route Ethernet frames, mvpnet uses a fixed mapping between
its MPI rank number, the guest VM IP address, and the guest VM Ethernet
hardware address.   Both IPv4 ARP and Ethernet broadcast operations are
supported by mvpnet.

## Building mvpnet

Build prerequisites:
- MPI message passing interface library (e.g. [MPICH](https://www.mpich.org/),
   [MVAPICH](https://mvapich.cse.ohio-state.edu/),
   [OpenMPI](https://www.open-mpi.org/))
- a POSIX-like system with the usual linux/unix C compiler and build tools

The mvpnet and wrapper programs are usually built using
[cmake](https://cmake.org/), but they also can be built manually.
To build using cmake, unpack the mvpnet source tree, make a build
directory, ensure that MPI is either in your path or its location
is specified in either `CMAKE_INSTALL_PREFIX` or `CMAKE_PREFIX_PATH`,
and then run `cmake` followed by `make`.

Note that `CMAKE_INSTALL_PREFIX` is used to tell cmake where to install
mvpnet.   `CMAKE_PREFIX_PATH` is used to specify additional directories
beyond PATH and `CMAKE_INSTALL_PREFIX` to search for external packages
(like MPI).

Example build that installs in `/tmp/mvp` and uses MPI from `/usr/mpich`:
<pre>
% cd /usr/local/src/mvpnet
% ls
CMakeLists.txt  monwrap         mvpnet          README.md       utilfns
% mkdir build
% cd build
% cmake -DCMAKE_INSTALL_PREFIX=/tmp/mvp \
	-DCMAKE_PREFIX_PATH=/usr/mpich ..
-- The C compiler identification is AppleClang 17.0.0.17000013
-- Detecting C compiler ABI info
-- Detecting C compiler ABI info - done
-- Check for working C compiler: /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/cc - skipped
-- Detecting C compile features
-- Detecting C compile features - done
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD - Success
-- Found Threads: TRUE
-- Found MPI_C: /usr/mpich/lib/libmpi.dylib (found version "4.1")
-- Found MPI: TRUE (found version "4.1")
-- Configuring done (0.5s)
-- Generating done (0.0s)
-- Build files have been written to: /usr/local/src/mvpnet/build
% make
[  6%] Building C object utilfns/CMakeFiles/utilfns.dir/utilfns.c.o
[ 13%] Linking C static library libutilfns.a
[ 13%] Built target utilfns
[ 20%] Building C object monwrap/CMakeFiles/monwrap.dir/monwrap.c.o
[ 26%] Linking C executable monwrap
[ 26%] Built target monwrap
[ 33%] Generating mvp_mlog.h
[ 40%] Building C object mvpnet/CMakeFiles/mvpnet.dir/fbufmgr.c.o
[ 46%] Building C object mvpnet/CMakeFiles/mvpnet.dir/fdio_thread.c.o
[ 53%] Building C object mvpnet/CMakeFiles/mvpnet.dir/mlog.c.o
[ 60%] Building C object mvpnet/CMakeFiles/mvpnet.dir/mpi_thread.c.o
[ 66%] Building C object mvpnet/CMakeFiles/mvpnet.dir/mvp_queuing.c.o
[ 73%] Building C object mvpnet/CMakeFiles/mvpnet.dir/mvp_stats.c.o
[ 80%] Building C object mvpnet/CMakeFiles/mvpnet.dir/mvpnet.c.o
[ 86%] Building C object mvpnet/CMakeFiles/mvpnet.dir/pktfmt.c.o
[ 93%] Building C object mvpnet/CMakeFiles/mvpnet.dir/qemucli.c.o
[100%] Linking C executable mvpnet
[100%] Built target mvpnet
% make install
[ 13%] Built target utilfns
[ 26%] Built target monwrap
[100%] Built target mvpnet
Install the project...
-- Install configuration: "Release"
-- Installing: /tmp/mvp/bin/monwrap
-- Installing: /tmp/mvp/bin/mvpnet
%
</pre>

To build manually (without cmake) you can invoke the C compiler
and `mpicc` directly from the command line as shown below.  Note
that you have to manually run `mkmloghdr.sh` to generate
`mvpnet/mvp_mlog.h` before you compile mvpnet.  You can safely delete
`mvpnet/mvp_mlog.h` after compiling.

<pre>
% ls
CMakeLists.txt  monwrap         mvpnet          README.md       utilfns
% cc -O -Iutilfns -o /tmp/mvp/bin/monwrap monwrap/monwrap.c utilfns/*.c
% ./mvpnet/mkmloghdr.sh > mvpnet/mvp_mlog.h
%
% mpicc -O -Iutilfns -o /tmp/mvp/bin/mvpnet utilfns/*.c mvpnet/*.c
%
% rm mvpnet/mvp_mlog.h
</pre>

## Running mvpnet

### Runtime prerequisites

- a compiled version of mvpnet and monwrap (see above)
- qemu 7.2 or later (must support `-netdev socket` or `-netdev dgram`)
  - allow qemu kvm access if possible (`-enable-kvm`) to cut emulator overhead
- a way to launch MPI jobs (mpirun w/host file, slurm salloc/srun/sbatch, etc.)
- a qemu-compatible bootable guest OS disk image file to run.  More information
about images can be found in the [examples](#example-usage) section.

### Command line options

**Note:** mvpnet is a work in progress -- command line flags may change.

Some mvpnet flag values can be prefixed with an optional MPI rank
specification to indicate that they are only to be applied to a specific
set of ranks.   A rank specification is a comma separated list of rank
ranges followed by a colon.   For example:

<pre>
-C 1          # enable console log on all ranks
              # (no rank specification)

-C 0,3,7-9:1  # enable console logs only on ranks 0, 3, 7, 8, and 9
</pre>
Flags marked with a '*' below accept a rank specification.

The main flags are:
<pre>
        # images to use, job id, what type of socket to use
	-i [img]   *image spec to load
	-c [val]   *cloud-init enable (cfg img file or dir)
	-j [id]     job id/name (added to log/socket names)
	-n [nett]   net type (stream or dgram) (def=stream)

        # configure emulation/guest vm size
	-k [val]    kvm on (1) or off (0) (def=1)
	-m [mb]    *guest VM memory size in mb (def=4096)

        # external programs we run (honors PATH)
	-M [wrap]   monwrap prog name (def=monwrap)
	-q [qemu]   qemu command (def=qemu-system-x86_64)

        # directories we use
	-r [dir]    runtime dir to copy image to (def=none)
	-s [dir]    socket directory (def=/tmp)
	-t [dir]    tftp dir (enables tftpd)
	-u [usr]    username on guest (for ssh)
	-d [dom]    domain (def=load from resolv.conf)
</pre>

Logging-related flags are:
<pre>
	-l [dir]    log file directory (default=/tmp)
	-g          have rank0 dump global stats to file at exit

        # what logs to enable
	-C [val]   *console output log on (1) or off (0) (def=0)
	-L [val]   *mpv mlog on (1) or off (0) (def=0)
	-w [val]   *wrapper log on (1) or off (0) (def=0)

        # in memory log size, logging level
	-B [sz]    *mlog msgbuf size (bytes, def=4096)
	-D [pri]   *mlog default priority (def=WARN)
	-S [pri]    mlog stderr priority (def=CRIT)
	-X [mask]  *mlog mask to set after defaults
</pre>

The `-i` image flag is the most important, as it tells mvpnet
what image the guest VM should boot.  It can also be used
to add additional storage devices to the guest VM (beyond
the boot drive) by adding additional `-i` flag.

There are two formats for `-i`:

<pre>
 -i image-file,prop1=val1,prop2=val2,...

 -i file=spec,prop1=val1,prop2=val2,...
</pre>

The first form starts with an image filename, while
the second consists only of `property=value` pairs (where one
of the properties allowed is `file=`).   The first form has additional
processing by mvpnet, while the second form is passed directly
through to qemu as-is.

The first form supports a `mvpctl=` property value to
control how mvpnet handles the image-file.   The mvpctl
property is a string consisting of a set of flag characters:
<pre>
c - copy image to rundir (-r must be set)
j - add job to filename in rundir
r - add rank to filename in rundir
d - remove image from rundir at exit
J - add job to image filename (-i)
R - add rank to image filename (-i)
D - remove image (-i) at exit
p - write-protect image file (read-only=on)
s - open file in snapshot mode (snapshot=on)
note: default mvpctl is 'cjrd' if '-r' is set
       otherwise the default is '' if '-r' is not set
example: '-i foo.img,mvpctl=s' run direct w/snapshot
</pre>

### Interacting with the mvpnet hosts

TODO:
ssh from host to one guest

ssh between guests

## Example usage

There are two general methods for configuring VM disk images for use with
mvpnet:
1. `cloud-init` configuration: Use a cloud-aware OS image preinstalled to use
the [cloud-init](https://cloud-init.io/) configuration system.
This method has a significant advantage in that there is
a readily available selection of
[official images from OS vendors](https://docs.openstack.org/image-guide/obtain-images.html).
Furthermore, using these images with mvpnet does not require any special
privileges in the host operating system.

1. Generate your own preconfigured guest OS disk image either manually or
with third-party tools.  This method can provide greater control over the
construction and configuration of the guest OS, but requires elevated
privileges in the host OS.  These privileges may not be available to
users in the mvpnet environment.

Demonstrations of both of these methods are included below.

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

### Manual disk image preparation
There are many ways to generate your own bootable guest OS disk image to run
with qemu, either on its own or under mvpnet.   For example, you can
download a standard bootable Linux distribution install media file, boot
it in a VM (e.g., as a cdrom drive), and install a fresh version of Linux
into a virtual disk file for use with mvpnet.

Here's how to run a demo of mvpnet with an Ubuntu cloud-init based image
with the current version of mvpnet.  This is just one example of how

First, download a cloud-init image from https://cloud-images.ubuntu.com/ ...
for example, a Ubuntu 24.04 image can can found here:

https://cloud-images.ubuntu.com/minimal/releases/noble/release/

e.g. grab [ubuntu-24.04-minimal-cloudimg-amd64.img](https://cloud-images.ubuntu.com/minimal/releases/noble/release/ubuntu-24.04-minimal-cloudimg-amd64.img).

Second, create a cloud-init configuration directory for the
guest VMs with the following two files in it.   The file "meta-data"
should contain:
<pre>
{
"instance-id": "iid-local01",
"dsmode": "local"
}
%
</pre>

And the file "user-data" should contain:
<pre>
#cloud-config
#
user:
  name: user
  ssh_authorized_keys:
    - ssh-rsa AAAAB3Nzazaya...
    - ssh-rsa AAAAB3NzaC1yc...
  sudo: 'ALL=(ALL) NOPASSWD:ALL'
</pre>

Where "user" is the login name you want to use on the host (typically
your own account's name) and the ssh-rsa lines are appropriate ssh keys
from your `~/.ssh/authorized_hosts` file.

So if you created the cloud-init config directory in ~/tmp/cloudinit
it should look like:
<pre>
% ls ~/tmp/cloudinit
meta-data  user-data
%
</pre>

The cloud-init image will mount this directory as a virtual FAT
filesystem and load the config from these files.

Assuming you've compiled mvpnet and put it in your path, you
can start a 2 node mvpnet test job with something like:

<pre>
mkdir -p /tmp/mvp

exec mpirun -n 2 \
	./mvpnet/mvpnet -C 1 \
	-c ~/tmp/cloudinit \
	-i ~/tmp/ubuntu-24.04-minimal-cloudimg-amd64.img,mvpctl=s \
	-l /tmp/mvp \
	-M ./monwrap/monwrap -r /tmp/mvp -s /tmp/mvp  \
        -g -L 1 -D INFO -S INFO \
	-n dgram foo
</pre>

You can login to the first VM on the host system using "ssh -p 2200 localhost"
and login to the second VM using "ssh -p 2201 localhost" on the host system.

To test the MPI-based networking between the two VMs, you currently
must manually bring up the network on each.   Open two windows on
the host system and use the two ssh commands noted above to login
to each VM.   The guest on port 2200 is the rank 0 guest and the
guest on port 2201 is the rank 1 guest.

On the rank 0 guest, get a root shell and bring up the ens3 interface:
<pre>
% ssh -p 2201 localhost
...
:~$ sudo -H /bin/bash
# ip addr add 10.0.0.0/8 dev ens3
# ip link set ens3 up
</pre>

On the rank 1 guest, repeat the above with the address 10.0.0.1/8 instead
of 10.0.0.0.

XXX: ssh keys not right for logging in between 10.0.0.0 and 10.0.0.1.
ssh will connect over the MPI network, but you will not be able to login.

To shutdown the mvpnet demo you must halt and power off both VMs.
run "halt -p" from the root shell on both VMs.

