mds_HOST=${mds_HOST:-mdev4}
mdsfailover_HOST=${mdsfailover_HOST:-mdev5}
ost1_HOST=${ost1_HOST:-mdev2}
ost2_HOST=${ost2_HOST:-mdev3}
EXTRA_OSTS=${EXTRA_OSTS:-mdev7}
client_HOST=client
LIVE_CLIENT=${LIVE_CLIENT:-mdev6}
# This should always be a list, not a regexp
FAIL_CLIENTS=${FAIL_CLIENTS:-mdev8}
#FAIL_CLIENTS=${FAIL_CLIENTS:-""}
SINGLEMDS=${SINGLEMDS:-"mds"}

NETTYPE=${NETTYPE:-tcp}

TIMEOUT=${TIMEOUT:-30}
PTLDEBUG=${PTLDEBUG:-0x3f0400}
SUBSYSTEM=${SUBSYSTEM:- 0xffb7e3ff}
MOUNT=${MOUNT:-"/mnt/lustre"}
#UPCALL=${CLIENT_UPCALL:-`pwd`/replay-single-upcall.sh}

MDSDEV=${MDSDEV:-/dev/sda1}
MDSSIZE=${MDSSIZE:-50000}
MDSJOURNALSIZE=${MDSJOURNALSIZE:-0}

OSTDEV=${OSTDEV:-$TMP/ost%d-`hostname`}
OSTSIZE=${OSTSIZE:=500000}
OSTJOURNALSIZE=${OSTJOURNALSIZE:-0}

FSTYPE=${FSTYPE:-ext3}
STRIPE_BYTES=${STRIPE_BYTES:-1048576} 
STRIPES_PER_OBJ=${STRIPES_PER_OBJ:-0}

FAILURE_MODE=${FAILURE_MODE:-HARD} # or HARD
POWER_DOWN=${POWER_DOWN:-"powerman --off"}
POWER_UP=${POWER_UP:-"powerman --on"}

PDSH="pdsh -S -w "