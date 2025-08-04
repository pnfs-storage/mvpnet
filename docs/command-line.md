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

