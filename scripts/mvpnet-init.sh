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

# these are gross...
mvpmac2num () {
  # take the special mvp format and convert it to a number
  echo ${1} | tr ':' ' ' | while read a b c; do
    sa=$(($((a))<<16))
    sb=$(($((b))<<8))
    sc=$(($((c))))
    echo $(($sa+$sb+$sc))
  done
}

num2mvpmac () {
  # take a number and convert it to the special format suitable for mvp mac or ip
  a=$(printf "%02d" $(($((${1}>>16))&0xff)))
  b=$(printf "%02d" $(($((${1}>>8))&0xff)))
  c=$(printf "%02d" $(($((${1}))&0xff)))
  echo "${a}:${b}:${c}"
}

ip -br link > /dev/null || die "cannot open ip link"

eval $(ip -br l | while read iface status mac rest; do
  if (echo ${mac} | grep -q "^52:56"); then
    echo wsize=$(mvpmac2num $(echo ${mac} | cut -f4-6 -d":"))
    echo useriface=${iface}
  elif (echo ${mac} | grep -q "^52:55"); then
    echo rank=$(mvpmac2num $(echo ${mac} | cut -f4-6 -d":"))
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
    ip="$(printf "10.%d.%d.%d" $(num2mvpmac ${i}|tr ':' ' '))"
    echo ${ip} ${lcv}
  done) > /etc/hosts || die "couldn't write /etc/hosts"

# and hosts.mpi
for i in $(seq 0 $((${wsize}-1))); do
  lcv=$(printf "n%04d" ${i})
  echo "${lcv}"
done > /etc/hosts.mpi || die "couldn't write /etc/hosts.mpi"

# ensure MPI interface is up if we found one
if [ -n ${mvpiface} ]; then
  ip -br addr show dev ${mvpiface} > /dev/null || die "cannot open ip inet"
  addrs=$(ip -br addr show dev ${mvpiface} | awk '{print $3}')
  if [ -n ${addrs} ]; then
    myaddr=$(printf "10.%d.%d.%d/8" $(num2mvpmac ${rank}|tr ':' ' '))
    echo "assign addr ${myaddr} to ${mvpiface}"
    ip addr add ${myaddr} dev ${mvpiface} || die "ip failed"
  else
    echo "${mvpiface} already has an ip address"
  fi
  echo "ensuring ${mvpiface} is up"
  ip link set ${mvpiface} up || die "ip failed"
fi

echo "mvpnet-init done!"

exit 0
