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
# ip addr add 10.0.0.1/8 broadcast 10.255.255.255 dev ens3
# ip link set ens3 up
</pre>

On the rank 1 guest, repeat the above with the address 10.0.0.2/8 instead
of 10.0.0.1.

XXX: ssh keys not right for logging in between 10.0.0.1 and 10.0.0.2.
ssh will connect over the MPI network, but you will not be able to login.

To shutdown the mvpnet demo you must halt and power off both VMs.
run "halt -p" from the root shell on both VMs.

