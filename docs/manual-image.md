### Manual disk image preparation
There are many ways to generate your own bootable guest OS disk image
to run with qemu, either on its own or under mvpnet.   For example, you
can download a standard bootable Linux distribution install media file,
boot it in a VM (e.g., as a cdrom drive), and install a fresh version of
Linux into a virtual disk file for use with mvpnet.  You could also use
a tool like [`virt-builder`](https://libguestfs.org/virt-builder.1.html)
to create a disk image from scratch.

This document provides a minimal demo of how an existing Ubuntu cloud-init
based image can be prepared for use with the current version of mvpnet.
More extensive customization instructions are beyond the scope of this tool.

1. Download a cloud-init enabled image from https://cloud-images.ubuntu.com/ ...
for example, an Ubuntu 24.04 image can can found here:
[ubuntu-24.04-minimal-cloudimg-amd64.img](https://cloud-images.ubuntu.com/minimal/releases/noble/release/ubuntu-24.04-minimal-cloudimg-amd64.img).

1. Next, copy the image to a preparation area to do additional configuration:
    ```
    mkdir -p /tmp/prep
    cp ubuntu-24.04-minimal-cloudimg-amd64.img /tmp/prep/ub24-min.img
    ```

1. Create a cloud-init configuration directory for the guest VMs with two
files in it.
    ```
    mkdir -p /tmp/prep/cloudcfg
    cd /tmp/prep/cloudcfg/
    ```

    The file named `meta-data` should contain:
    <pre>
    {
    "instance-id": "iid-local01",
    "dsmode": "local"
    }
    </pre>

    And the file named `user-data` should contain:
    ```
    #cloud-config
    #
    user:
      name: USER
      ssh_authorized_keys:
        - ssh-rsa AAAAB3NzaC1... (one of USER's ssh public keys)
        - ssh-rsa AAAAB3NzaC1... (another of USER's ssh public keys)
      sudo: 'ALL=(ALL) NOPASSWD:ALL'
    ```

    Where "USER" is the actual login name you want to use on the guest (typically
    your own account's name on the host) and the ssh-rsa lines are appropriate ssh keys
    from your `~/.ssh/authorized_hosts` file.  It is important to ensure that
    ssh access both from the host system to the guest VM and from one guest VM
    to another in the same job is working as this is a prerequisite for the
    rest of the mvpnet system to work.
 
    So if you created the cloud-init config directory in `/tmp/prep/cloudcfg`
    it should look like:
    ```
    $ ls /tmp/prep/cloudcfg
    meta-data  user-data
    $
    ```

    The cloud-init image will mount this directory as a virtual FAT
    filesystem and load the configuration from these files.

1. Boot the above image directly in qemu with the cloud configuration in order
to do more customization within the running VM:
    ```
    qemu-system-x86_64 -serial mon:stdio -nographic -display none -vga none \
     -cpu host \
     -enable-kvm -m 4096M \
     -drive file=/tmp/prep/ub24-min.img,media=disk,if=virtio \
     -drive file=fat:/tmp/prep/cloudcfg,media=disk,if=virtio,file.label=cidata,snapshot=on \
     -netdev user,id=usernet,net=192.168.1.0/24,hostfwd=tcp:127.0.0.1:2200-:22 \
     -device virtio-net-pci,netdev=usernet,mac=52:56:00:00:00:01
    ```

1. Login to the VM using ssh forwarded from port on localhost:
    ```
    # should not ask for a password, since cloud-init created
    # my account and put my ssh key in
    ssh -p 2200 localhost

    # should not ask for password, since we configured sudo with cloud init
    sudo -H /bin/bash
    ```

1. Add a compiler, MPI, and other software to the image.  We are going to assume
that you prepare the image on either an open network or one with a web proxy
that lets you get to the packages.  Once the image is set up with what you
need, you hopefully won't need external internet access anymore.
    ```
    # on our systems that need to set a web proxy:

    export https_proxy=http://proxy.example.edu:3128/
    export http_proxy=http://proxy.example.edu:3128/
    ```

    Use the system's package manager to install what you need.  On Ubuntu that
    is an apt command like:
    ```
    apt-get update
    apt-get install -y net-tools tcsh vim tftp-hpa iputils-ping \
                    mpich policykit-1
    ```

1. Set up an ssh key for root
    ```
    cd ~root/.ssh
    ssh-keygen
    # take defaults, empty pass phrase
    cat id_ed25519.pub >> authorized_keys
    ```

1. Set up the normal user's ssh key
    ```
    exit # drop root shell
    cd ~/.ssh
    ssh-keygen
    # take defaults, empty pass phrase
    cat id_ed25519.pub >> authorized_keys
    ```

1. As root, edit `/etc/ssh/ssh_config`
    ```
    # add uncommented line (otherwise mpich fails):
        StrictHostKeyChecking no
    ```

1. Remove `/etc/hostname` so that the guest gets its hostname from qemu dhcp:
    ```
    sudo rm -f /etc/hostname
    ```

1. Assuming you've compiled mvpnet and put it in your path, you
can start a 2 node mvpnet test job with something like:
    ```
    mkdir -p /tmp/mvp

    exec mpirun -n 2 \
	    ./mvpnet/mvpnet -C 1 \
	    -c /tmp/prep/cloudcfg \
	    -i /tmp/prep/ub24-min.img,mvpctl=s \
	    -l /tmp/mvp \
	    -M ./monwrap/monwrap -r /tmp/mvp -s /tmp/mvp  \
            -g -L 1 -D INFO -S INFO \
	    -n dgram foo
    ```

    You can login to the first VM on the host system using "ssh -p 2200 localhost"
    and login to the second VM using "ssh -p 2201 localhost" on the host system.

    To test the MPI-based networking between the two VMs, you currently
    must manually bring up the network on each.   Open two windows on
    the host system and use the two ssh commands noted above to login
    to each VM.   The guest on port 2200 is the rank 0 guest and the
    guest on port 2201 is the rank 1 guest.

    On the rank 0 guest, get a root shell and bring up the ens3 interface:
    ```
    $ ssh -p 2201 localhost
    ...
    :~$ sudo -H /bin/bash
    # ip addr add 10.0.0.1/8 broadcast 10.255.255.255 dev ens3
    # ip link set ens3 up
    ```

    On the rank 1 guest, repeat the above with the address 10.0.0.2/8 instead
    of 10.0.0.1.

    XXX: ssh keys not right for logging in between 10.0.0.1 and 10.0.0.2.
    ssh will connect over the MPI network, but you will not be able to login.

    To shutdown the mvpnet demo you must halt and power off both VMs.
    run "halt -p" from the root shell on both VMs.

