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

### [Command line options](docs/command-line.md)

### [Interacting with the mvpnet hosts](docs/guest-interaction.md)

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

Demonstrations of both of these methods are referenced below.

### [`cloud-init` configuration](docs/cloud-init.md)

### [Manual disk image preparation](docs/manual-image.md)

