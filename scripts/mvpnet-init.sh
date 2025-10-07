#!/bin/sh
# mvpnet-init  bring up mvpnet MPI network (if present) and init host files
# 2025-07-28  crd@andrew.cmu.edu
#
# this is a portable shell rewrite of chuck@ece.cmu.edu's original version
# since some distribution's cloud images no longer include perl.
#

umask 022

die () {
  echo "FATAL: $*" 1>&2
  exit 1
}

mvpmac2rank () {
  # take the special mvp mac format and convert it to a rank number
  echo ${1} | tr ':' ' ' | while read a b c; do
    sa=$((0x${a}<<16))
    sb=$((0x${b}<<8))
    sc=$((0x${c}))
    echo $((${sa}+${sb}+${sc}))
  done
}

rank2mvpip () {
  # take a rank number and convert it to the special mvp ip
  a=$((${1}>>16&0xff))
  b=$((${1}>>8&0xff))
  c=$((${1}&0xff))
  echo "10.${a}.${b}.${c}"
}

# define mac and ip addr functions for different systems
if [ $(uname) = "Linux" ]; then
  getmacs() {
    ip -br link | awk '{print $1, $3}' || die "cannot open ip link"
  }
  getaddr() {
    ip -br addr show dev ${1} > /dev/null || die "cannot open ip inet"
  }
  setaddr() {
    ip addr add ${1} broadcast 10.255.255.255 dev ${2} || die "setting ip failed"
  }
  ifup() {
    ip link set ${1} up || die "ip up failed"
  }
elif [ $(uname) = "FreeBSD" ]; then
  getmacs() {
    ifconfig > /dev/null || die "cannot get ifconfig"
    for interface in $(ifconfig -l ether); do
      ifconfig ${interface} ether | xargs | awk '{print $1, $9}' | sed 's/://'
    done
  }
  getaddr() {
    ifconfig -f inet:cidr ${1} inet | xargs | awk '{print $9}' || die "cannot open ifconfig inet"
  }
  setaddr() {
    ifconfig ${2} inet ${1} broadcast 10.255.255.255 up || die "setting ip failed"
  }
  ifup() {
    ifconfig ${1} up || die "ifconfig up failed"
  }
else
  die "don't know how to get network config on this system"
fi

eval $(getmacs | while read iface mac; do
  if (echo ${mac} | grep -q "^52:56"); then
    echo wsize=$(mvpmac2rank $(echo ${mac} | cut -f4-6 -d":"))
    echo useriface=${iface}
  elif (echo ${mac} | grep -q "^52:55"); then
    echo rank=$(mvpmac2rank $(echo ${mac} | cut -f4-6 -d":"))
    echo mvpiface=${iface}
  fi
done)

if [ ! $((${wsize})) -gt 0 ]; then
  die "no world size found"
else
  echo "wsize=${wsize}"
fi

if [ -z ${mvpiface} ]; then
  echo "note: no MPI network interface detected"
else
  echo "rank=${rank}"
  echo "mvpiface=${mvpiface}"
fi

if [ $((${rank})) -gt $((${wsize})) ]; then
  die "rank cannot be larger than world size"
fi

# update hosts files based on world size
cp /etc/hosts /tmp/mvphosts || die "couldn't write temporary hosts file"
(grep -vE "^#wsize: |^10\." /tmp/mvphosts; echo "#wsize: ${wsize}";
  for i in $(seq 0 $((${wsize}-1))); do
    lcv=$(printf "n%04d" ${i})
    # the real IP address is one bit more than what we get from the rank
    n=$((${i}+1))
    ip=$(rank2mvpip ${n})
    echo ${ip} ${lcv}
  done) > /etc/hosts || die "couldn't write /etc/hosts"

# and hosts.mpi
for i in $(seq 0 $((${wsize}-1))); do
  lcv=$(printf "n%04d" ${i})
  echo "${lcv}"
done > /etc/hosts.mpi || die "couldn't write /etc/hosts.mpi"

# ensure MPI interface is up if we found one
if [ -n ${mvpiface} ]; then
  addrs=$(getaddr ${mvpiface})
  if [ -n ${addrs} ]; then
    # the real IP address is one bit more than what we get from the rank
    n=$((${rank}+1))
    myaddr=$(rank2mvpip ${n})/8
    echo "assign addr ${myaddr} to ${mvpiface}"
    setaddr ${myaddr} ${mvpiface}
  else
    echo "${mvpiface} already has an ip address"
  fi
  echo "ensuring ${mvpiface} is up"
  ifup ${mvpiface}
fi

echo "mvpnet-init done!"

exit 0
