# mvpnet

## building mvpnet

Build prerequisites:
- MPI message passing interface library (e.g. [MPICH](https://www.mpich.org/),
   [MVAPICH](https://mvapich.cse.ohio-state.edu/),
   [OpenMPI](https://www.open-mpi.org/))
- a POSIX-like system with the usual linux/unix C compiler and build tools

The mvpnet and wrapper programs are usually built using
[cmake](https://cmake.org/) (but they also can be built manually).
To build using cmake, unpack the mvpnet source tree, make a build
directory, ensure that MPI is either in your path or it's location
is specified in either CMAKE_INSTALL_PREFIX or CMAKE_PREFIX_PATH,
and then run cmake followed by make.

Note that CMAKE_INSTALL_PREFIX is used to tell cmake where to install
mvpnet.   CMAKE_PREFIX_PATH is used to specify additional directories
beyond PATH and CMAKE_INSTALL_PREFIX to search for external packages
(like MPI).

Example builds that installs in /tmp/mvp and uses MPI from /usr/mpich:
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
and mpicc directly from the command line as shown below.  Note
that you have to manually run mkmloghdr.sh to generate
mvpnet/mvp_mlog.h before you compile mvpnet.

<pre>
% ls
CMakeLists.txt  monwrap         mvpnet          README.md       utilfns
% cc -O -Iutilfns -o /tmp/mvp/bin/monwrap monwrap/monwrap.c utilfns/*.c
% ./mvpnet/mkmloghdr.sh > mvpnet/mvp_mlog.h
%
% mpicc -O -Iutilfns -o /tmp/mvp/bin/mvpnet utilfns/*.c mvpnet/*.c
%
</pre>

