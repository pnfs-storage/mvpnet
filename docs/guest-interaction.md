### Interacting with the mvpnet hosts

Each guest VM started via mvpnet has an account that the user can access via
ssh in various ways:
 - ssh to the control interface via `localhost` of the corresponding host
that the VM is running on (e.g., `ssh -p 2200 localhost`).  The port number is
determined by the MPI rank of the guest.
 - ssh within the mvpnet MPI network (IPs 10.x.y.z where x.y.z is an
   encoding of the rank)

The ssh keys should be setup so that no password is required for either of the
two ssh methods described above.

Once you are logged into your account on a guest VM you should:
 - have passwordless "sudo" access to root

 - be able to ssh to your account on other VMs in the same MPI job over the MPI
network (10.0.0.0/8) using an ssh key and without having to type a password

 - if you have a root shell, the root shell should also be able to ssh to other
VMs over the MPI network using an ssh key without having to type a password.
The user and root ssh keys can be the same - it doesn't matter since this is all
running on the completely private MPI network under the control of the user.

The mvpnet configuration process for the VMs should assign working hostnames
for all ranks on the network.  These are stored statically in `/etc/hosts` on
each VM.  For example, a job with world size of 4 results in:
```
$ cat /etc/hosts
127.0.0.1 localhost

# The following lines are desirable for IPv6 capable hosts
::1 ip6-localhost ip6-loopback
fe00::0 ip6-localnet
ff00::0 ip6-mcastprefix
ff02::1 ip6-allnodes
ff02::2 ip6-allrouters
ff02::3 ip6-allhosts
#wsize: 4
10.0.0.1 n0000
10.0.0.2 n0001
10.0.0.3 n0002
10.0.0.4 n0003
```

The mvpnet network configuration process also generates a hostsfile,
`/etc/hosts.mpi`, with the names of all of the VMs in the job.  This can be
used by MPI with the `mpirun` command, e.g.,
`mpirun -n 2 -f /etc/hosts.mpi hostname`.

It may also be desirable to setup some form of parallel ssh mechanism within the
mvpnet guests.  The [pdsh](https://github.com/chaos/pdsh) and
[parallel-ssh](https://parallel-ssh.org/) programs are popular examples and
can be added through most package management systems.  It is only necessary to
install such packages into one of the guest VMs in order to use them from the
mvpnet MPI network.  For example (note that the specific package names and
methods to install them will vary between guest operating systems):
```
[root@n0002 ~]# dnf install pssh pdsh pdsh-rcmd-ssh pdsh-mod-genders
...
[user@n0002 ~]$ pssh -h /etc/hosts.mpi -i hostname
[1] 16:27:53 [SUCCESS] n0000
n0000
[2] 16:27:53 [SUCCESS] n0003
n0003
[3] 16:27:53 [SUCCESS] n0002
n0002
[4] 16:27:54 [SUCCESS] n0001
n0001
[user@n0002 ~]$ pdsh -R ssh -F /etc/hosts.mpi -a hostname
n0001: n0001
n0000: n0000
n0003: n0003
n0002: n0002
[user@n0002 ~]$ sudo pdsh -R ssh -F /etc/hosts.mpi -a id
n0002: uid=0(root) gid=0(root) groups=0(root) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023
n0000: uid=0(root) gid=0(root) groups=0(root) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023
n0003: uid=0(root) gid=0(root) groups=0(root) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023
n0001: uid=0(root) gid=0(root) groups=0(root) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023
```

