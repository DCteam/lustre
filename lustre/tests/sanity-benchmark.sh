#!/bin/bash
#
# Test basic functionality of the filesystem using simple
# benchmarks.
#

set -e

LUSTRE=${LUSTRE:-$(cd $(dirname $0)/..; echo $PWD)}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}

MAX_THREADS=${MAX_THREADS:-20}
RAMKB=`awk '/MemTotal:/ { print $2 }' /proc/meminfo`
if [ -z "$THREADS" ]; then
	THREADS=$((RAMKB / 16384))
	[ $THREADS -gt $MAX_THREADS ] && THREADS=$MAX_THREADS
fi
SIZE=${SIZE:-$((RAMKB * 2))}
RSIZE=${RSIZE:-512}

DEBUG_LVL=${DEBUG_LVL:-0}
DEBUG_OFF=${DEBUG_OFF:-"eval lctl set_param debug=\"$DEBUG_LVL\""}
DEBUG_ON=${DEBUG_ON:-"eval lctl set_param debug=0x33f0484"}

PIOSBIN=${PIOSBIN:-$(which pios 2> /dev/null || true)}

pios_THREADCOUNT=${pios_THREADCOUNT:-"1,8,40"}
[ "$SLOW" = "no" ] && pios_THREADCOUNT=8

pios_REGIONCOUNT=${pios_REGIONCOUNT:-1024}
pios_CHUNKSIZE=${pios_CHUNKSIZE:-1M}
pios_REGIONSIZE=${pios_REGIONSIZE:-8M}
pios_OFFSET=${pios_OFFSET:-16M}

[ "$SLOW" = "no" ] && EXCEPT_SLOW="iozone"

build_test_filter
check_and_setup_lustre

assert_DIR
rm -rf $DIR/[df][0-9]*

test_dbench() {
    if ! which dbench > /dev/null 2>&1 ; then
	skip_env "No dbench installed"
	return
    fi

    DBENCHDIR=$MOUNT/d0.$HOSTNAME
    mkdir -p $DBENCHDIR
    local SPACE=`df -P $MOUNT | tail -n 1 | awk '{ print $4 }'`
    DB_THREADS=$((SPACE / 50000))
    [ $THREADS -lt $DB_THREADS ] && DB_THREADS=$THREADS
    
    $DEBUG_OFF
    myUID=$RUNAS_ID
    myRUNAS=$RUNAS
    FAIL_ON_ERROR=false check_runas_id_ret $myUID $myUID $myRUNAS || { myRUNAS="" && myUID=$UID; }
    chown $myUID:$myUID $DBENCHDIR
    local duration=""
    [ "$SLOW" = "no" ] && duration=" -t 120"
    if [ "$SLOW" != "no" -o $DB_THREADS -eq 1 ]; then
	$myRUNAS bash rundbench -D $DBENCHDIR 1 $duration || error "dbench failed!"
	$DEBUG_ON
    fi
    if [ $DB_THREADS -gt 1 ]; then
	$DEBUG_OFF
	$myRUNAS bash rundbench -D $DBENCHDIR $DB_THREADS $duration
	$DEBUG_ON
    fi
    rm -rf $DBENCHDIR
}
run_test dbench "dbench"

test_bonnie() {
    if ! which bonnie++ > /dev/null 2>&1; then
	skip_env "No bonnie++ installed"
	return 0
    fi
    BONDIR=$MOUNT/d0.bonnie
    mkdir -p $BONDIR
    $LFS setstripe -c -1 $BONDIR
    sync
    local MIN=`lctl get_param -n osc.*.kbytesavail | sort -n | head -n1`
    local SPACE=$(( OSTCOUNT * MIN ))
    [ $SPACE -lt $SIZE ] && SIZE=$((SPACE * 3 / 4))
    log "min OST has ${MIN}kB available, using ${SIZE}kB file size"
    $DEBUG_OFF
    myUID=$RUNAS_ID
    myRUNAS=$RUNAS
    FAIL_ON_ERROR=false check_runas_id_ret $myUID $myUID $myRUNAS || { myRUNAS="" && myUID=$UID; }
    chown $myUID:$myUID $BONDIR		
    $myRUNAS bonnie++ -f -r 0 -s$((SIZE / 1024)) -n 10 -u$myUID:$myUID -d$BONDIR
    $DEBUG_ON
}
run_test bonnie "bonnie++"

test_iozone() {
    if ! which iozone > /dev/null 2>&1; then
	skip_env "No iozone installed"
	return 0
    fi

    export O_DIRECT
    
    IOZDIR=$MOUNT/d0.iozone
    mkdir -p $IOZDIR
    $LFS setstripe -c -1 $IOZDIR
    sync
    local MIN=`lctl get_param -n osc.*.kbytesavail | sort -n | head -n1`
    local SPACE=$(( OSTCOUNT * MIN ))
    [ $SPACE -lt $SIZE ] && SIZE=$((SPACE * 3 / 4))
    log "min OST has ${MIN}kB available, using ${SIZE}kB file size"
    IOZONE_OPTS="-i 0 -i 1 -i 2 -e -+d -r $RSIZE"
    IOZFILE="$IOZDIR/iozone"
    IOZLOG=$TMP/iozone.log
		# $SPACE was calculated with all OSTs
    $DEBUG_OFF
    myUID=$RUNAS_ID
    myRUNAS=$RUNAS
    FAIL_ON_ERROR=false check_runas_id_ret $myUID $myUID $myRUNAS || { myRUNAS="" && myUID=$UID; }
    chown $myUID:$myUID $IOZDIR
    $myRUNAS iozone $IOZONE_OPTS -s $SIZE -f $IOZFILE 2>&1 | tee $IOZLOG
    tail -1 $IOZLOG | grep -q complete || \
	{ error "iozone (1) failed" && return 1; }
    rm -f $IOZLOG
    $DEBUG_ON
    
    # check if O_DIRECT support is implemented in kernel
    if [ -z "$O_DIRECT" ]; then
	touch $MOUNT/f.iozone
	if ! ./directio write $MOUNT/f.iozone 0 1; then
	    log "SKIP iozone DIRECT IO test"
	    O_DIRECT=no
	fi
	rm -f $MOUNT/f.iozone
    fi
    if [ "$O_DIRECT" != "no" -a "$IOZONE_DIR" != "no" ]; then
	$DEBUG_OFF
	$myRUNAS iozone -I $IOZONE_OPTS -s $SIZE -f $IOZFILE.odir 2>&1 | tee $IOZLOG
	tail -1 $IOZLOG | grep -q complete || \
	    { error "iozone (2) failed" && return 1; }
	rm -f $IOZLOG
	$DEBUG_ON
    fi

    SPACE=`df -P $MOUNT | tail -n 1 | awk '{ print $4 }'`
    IOZ_THREADS=$((SPACE / SIZE * 2 / 3 ))
    [ $THREADS -lt $IOZ_THREADS ] && IOZ_THREADS=$THREADS
    IOZVER=`iozone -v | awk '/Revision:/ {print $3}' | tr -d .`
    if [ "$IOZ_THREADS" -gt 1 -a "$IOZVER" -ge 3145 ]; then
	$LFS setstripe -c -1 $IOZDIR
	$DEBUG_OFF
	THREAD=1
	IOZFILE=" "
	while [ $THREAD -le $IOZ_THREADS ]; do
	    IOZFILE="$IOZFILE $IOZDIR/iozone.$THREAD"
	    THREAD=$((THREAD + 1))
	done
	$myRUNAS iozone $IOZONE_OPTS -s $((SIZE / IOZ_THREADS)) -t $IOZ_THREADS -F $IOZFILE 2>&1 | tee $IOZLOG
	tail -1 $IOZLOG | grep -q complete || \
	    { error "iozone (3) failed" && return 1; }
	rm -f $IOZLOG
	$DEBUG_ON
    elif [ $IOZVER -lt 3145 ]; then
	VER=`iozone -v | awk '/Revision:/ { print $3 }'`
	echo "iozone $VER too old for multi-thread test"
    fi
}
run_test iozone "iozone"

test_fsx() {
    FSX_SIZE=$SIZE
    FSX_COUNT=1000
    local SPACE=`df -P $MOUNT | tail -n 1 | awk '{ print $4 }'`
    [ $SPACE -lt $FSX_SIZE ] && FSX_SIZE=$((SPACE * 3 / 4))
    $DEBUG_OFF
    FSX_SEED=${FSX_SEED:-$RANDOM}
    rm -f $MOUNT/fsxfile
    $LFS setstripe -c -1 $MOUNT/fsxfile
    echo Using FSX_SEED=$FSX_SEED FSX_SIZE=$FSX_SIZE FSX_COUNT=$FSX_COUNT
    fsx -c 50 -p 1000 -S $FSX_SEED -P $TMP -l $FSX_SIZE \
	-N $(($FSX_COUNT * 100)) $MOUNT/fsxfile
    $DEBUG_ON
}
run_test fsx "fsx"


############################################################
# PIOS
#

iterpr_KMGT () {
    local str=$1
    local num=${str:0:${#str}-1}
    case ${str:${#str}-1} in
        k|K ) num=$((num << 10));; #
        m|M ) num=$((num << 20));; # emacs is confsued by the <<  and
        g|G ) num=$((num << 30));; # these comments help it out.
        t|T ) num=$((num << 40));; #
          * ) num=$str;;
    esac
    echo $num
}

space_check () {
    # space estimation
    # /* Adding 10% to total test size for filesystem overhead */
    #  size = size + (double)(size) * (double) (0.1);
    # 
    #  total_test_size = runarg->stream[n - 1].max_offset +
    #                            runarg->regionsize;

    local space=$(df -P $DIR | tail -n 1 | awk '{ print $4 }')
    local size=$(($(iterpr_KMGT $pios_REGIONCOUNT) * \
                  $(iterpr_KMGT $pios_OFFSET) + \
                  $(iterpr_KMGT $pios_REGIONSIZE) ))
    size=$(( size + size / 10 ))
    if [ $((space * 1024)) -le $size ]; then 
        echo "Need free space atleast $size, have $((space * 1024))"
        return 10
    fi
}

pios_setup() { 
    local testdir=$DIR/$tdir
    mkdir -p $testdir

    stripes=1
    [ "$1" == "--stripe" ] && stripes=-1
    $LFS setstripe $testdir -c $stripes
    echo "Test directory stripe count: $stripes"
}

pios_cleanup() {
    local rc=$1
    local testdir=$DIR/$tdir
    [ $rc = 0 ] && rm -rf $testdir
}

run_pios () {
    local testdir=$DIR/$tdir
    local cmd="$PIOSBIN  -t $pios_THREADCOUNT -n $pios_REGIONCOUNT \
                         -c $pios_CHUNKSIZE -s $pios_REGIONSIZE    \
                         -o $pios_OFFSET  $@ -p $testdir"
    
    if [ ! -d $testdir ]; then  
        error "No test directory created, setup_pios must have failed"
        return 20
    fi

    log "$cmd"

    local rc=0
    eval $cmd
    rc=$?

    return $rc
}

test_pios_ssf() {
    if  [ -z "$PIOSBIN" ]; then
        skip_env "$0 : pios not found PIOSBIN=$PIOSBIN"
	return
    fi

    local rc=0
    space_check || { skip_env "not enough space" && return 0; }
    pios_setup --stripe || return
    # bug 19657
    local old_PWD=$PWD
    cd $TMP
    run_pios || return
    run_pios  --verify || rc=$? 
    cd $old_PWD
    pios_cleanup $rc
    return $rc
}
run_test pios_ssf "pios shared single file"

test_pios_fpp() {
    if  [ -z "$PIOSBIN" ]; then
        skip_env "pios not found PIOSBIN=$PIOSBIN"
        return
    fi

    local rc=0
    space_check || { skip_env "not enough space" && return 0; }
    pios_setup || return
    # bug 19657
    local old_PWD=$PWD
    cd $TMP
    run_pios -L fpp || return
    run_pios -L fpp --verify || rc=$?
    cd $old_PWD
    pios_cleanup $rc
    return $rc
}
run_test pios_fpp "pios file per process"

equals_msg `basename $0`: test complete, cleaning up
check_and_cleanup_lustre
[ -f "$TESTSUITELOG" ] && cat $TESTSUITELOG && grep -q FAIL $TESTSUITELOG && exit 1 || true