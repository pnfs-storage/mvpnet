#!/usr/bin/perl
#
# mvpnet-init  bring up mvpnet MPI network (if present) and init host files
# 09-Jul-2025  chuck@ece.cmu.edu
#

# expected "ip -o -f link addr" output format:
#2: ens2: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP group default qlen 1000\    link/ether 52:56:00:00:00:01 brd ff:ff:ff:ff:ff:ff

use strict;
umask(022);

#
# savefile(file, joinstr, content1, content2,...): safely save data to a file.
# return 1 on success, undef on error.
#
sub savefile {
    my($file) = shift(@_);
    my($joinstr) = shift(@_);
    my($hand);

    # must have a filename (if already present, must be a file).
    return(undef) if ($file eq '' || (-e $file && ! -f $file));

    # return 1 if total success!
    if (open($hand, ">$file.new.$$")        &&
        print($hand join($joinstr, @_))     &&
        close($hand)                        &&
        rename("$file.new.$$", $file) ) {
        return(1);
    }

    # failure
    undef($hand);                               # will close, if open
    unlink("$file.new.$$");
    return(undef);
}


my($ip, $try, $type, $val, $wsize, $rank, $iface);
my($h, @hosts, $lcv, @mpihosts, $gotinet, $myaddr, $rv);

open($ip, "ip -o -f link addr|") || die "cannot open ip link";
while (<$ip>) {
    chop;
    next unless (/link.ether (\S+) brd /);
    $try = $1;
    next unless ($try =~ /^52:(\d\d):(\d\d):(\d\d):(\d\d):(\d\d)$/);
    $type = $1;
    $val = (int($3) << 16) || (int($4) << 8) || int($5);
    if ($type == 56) {
        $wsize = $val;
    } elsif ($type == 55 && /^\d+: (\w+): /) {
        $rank = $val;
        $iface = $1;
    }
}
close($ip);

die "no world size found" unless ($wsize > 0);
print "wsize=$wsize\n";
if (!defined($rank)) {
    print "note: no MPI network interface detected\n";
} else {
    print "rank=$rank\n";
    print "iface=$iface\n";
}

# update /etc/hosts based on world size
open($h, "</etc/hosts") || die "cannot open /etc/hosts";
while (<$h>) {
    next if (/^#wsize: /);
    next if (/^10\./);
    push(@hosts, $_);
}
close($h);
push(@hosts, "#wsize: $wsize\n");
for ($lcv = 0 ; $lcv < $wsize ; $lcv++) {
    push(@hosts, sprintf("10.%d.%d.%d\tn%04d\n", ($lcv >> 16) & 0xff,
          ($lcv >> 8) & 0xff, $lcv & 0xff, $lcv));
    push(@mpihosts, sprintf("n%04d\n", $lcv));
}

print "\n";
savefile("/etc/hosts", '', @hosts) || die "savefile /etc/hosts failed";
print "updated /etc/hosts\n";
savefile("/etc/hosts.mpi", '', @mpihosts) ||
                                      die "savefile /etc/hosts.mpi failed";
print "updated /etc/hosts.mpi\n";

# expected "ip -o -f inet addr" output format:
# ens3: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP group default qlen 1000\    link/ether 52:55:00:00:00:00 brd ff:ff:ff:ff:ff:ff\    altname enp0s3

# ensure MPI interface is up if we found one
if (defined($rank)) {
    open($ip, "ip -o -f inet addr|") || die "cannot open ip inet";
    while (<$ip>) {
        next unless (/^\d+:\s+(\w+)/);
        if ($1 eq $iface) {
            $gotinet++;
            last;
        }
    }
    close($ip);
    if ($gotinet == 0) {
        $myaddr = sprintf("10.%d.%d.%d/8", ($rank >> 16) & 0xff,
                          ($rank >> 8) & 0xff, $rank & 0xff);
        print "assign addr $myaddr to $iface\n";
        $rv = system "ip", "addr", "add", $myaddr, "dev", $iface;
        die "ip failed" if ($rv);
    } else {
        print "$iface already has an ip address\n";
    }
    print "ensuring $iface is up\n";
    $rv = system "ip", "link", "set", $iface, "up";
    die "ip failed" if ($rv);
}

print "mvpnet-init done!\n";

exit(0);
