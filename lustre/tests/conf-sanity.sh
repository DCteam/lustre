#!/bin/bash
# requirement:
#	add uml1 uml2 uml3 in your /etc/hosts

set -e

SRCDIR=`dirname $0`
PATH=$PWD/$SRCDIR:$SRCDIR:$SRCDIR/../utils:$PATH

LUSTRE=${LUSTRE:-`dirname $0`/..}
RLUSTRE=${RLUSTRE:-$LUSTRE}

. $LUSTRE/tests/test-framework.sh

init_test_env $@

. ${CONFIG:=$LUSTRE/tests/cfg/local.sh}

FORCE=${FORCE:-" --force"}

gen_config() {
	rm -f $XMLCONFIG

	add_mds mds --dev $MDSDEV --size $MDSSIZE
	add_lov lov1 mds --stripe_sz $STRIPE_BYTES\
	    --stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0
	add_ost ost --lov lov1 --dev $OSTDEV --size $OSTSIZE
	add_client client mds --lov lov1 --path $MOUNT
}

gen_second_config() {
	rm -f $XMLCONFIG

	add_mds mds2 --dev $MDSDEV --size $MDSSIZE
	add_lov lov2 mds2 --stripe_sz $STRIPE_BYTES\
	    --stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0
	add_ost ost2 --lov lov2 --dev $OSTDEV --size $OSTSIZE
	add_client client mds2 --lov lov2 --path $MOUNT2
}

start_mds() {
	echo "start mds service on `facet_active_host mds`"
	start mds --reformat $MDSLCONFARGS > /dev/null || return 94
}
stop_mds() {
	echo "stop mds service on `facet_active_host mds`"
	stop mds $@ > /dev/null || return 97 
}

start_ost() {
	echo "start ost service on `facet_active_host ost`"
	start ost --reformat $OSTLCONFARGS > /dev/null || return 95
}

stop_ost() {
	echo "stop ost service on `facet_active_host ost`"
	stop ost $@ > /dev/null || return 98 
}

mount_client() {
	local MOUNTPATH=$1
	echo "mount lustre on ${MOUNTPATH}....."
	zconf_mount $MOUNTPATH > /dev/null || return 96
}

umount_client() {
	local MOUNTPATH=$1
	echo "umount lustre on ${MOUNTPATH}....."
	zconf_umount $MOUNTPATH > /dev/null || return 97
}

manual_umount_client(){
	echo "manual umount lustre on ${MOUNTPATH}...."
	do_facet  client "umount $MOUNT"
}

setup() {
	start_ost
	start_mds
	mount_client $MOUNT 
}

cleanup() {
 	umount_client $MOUNT || return -200
	stop_mds  || return -201
	stop_ost || return -202
}

check_mount() {
	do_facet client "touch $DIR/a" || return 71	
	do_facet client "rm $DIR/a" || return 72	
	echo "setup single mount lustre success"
}

check_mount2() {
	do_facet client "touch $DIR/a" || return 71	
	do_facet client "rm $DIR/a" || return 72	
	do_facet client "touch $DIR2/a" || return 73	
	do_facet client "rm $DIR2/a" || return 74	
	echo "setup double mount lustre success"
}

build_test_filter

#create single point mountpoint

gen_config


test_0() {
	start_ost
	start_mds	
	mount_client $MOUNT  
	check_mount || return 41
	cleanup  
}
run_test 0 "single mount setup"

test_1() {
	start_ost
	echo "start ost second time..."
	start ost --reformat $OSTLCONFARGS > /dev/null 
	start_mds	
	mount_client $MOUNT
	check_mount || return 42
	cleanup 
}
run_test 1 "start up ost twice"

test_2() {
	start_ost
	start_mds	
	echo "start mds second time.."
	start mds --reformat $MDSLCONFARGS > /dev/null 
	
	mount_client $MOUNT  
	check_mount || return 43
	cleanup 
}
run_test 2 "start up mds twice"

test_3() {
        setup
	mount_client $MOUNT

	check_mount || return 44
	
 	umount_client $MOUNT 	
	cleanup  
}
run_test 3 "mount client twice"

test_4() {
	setup
	touch $DIR/$tfile || return 85
	stop_ost ${FORCE}

	# cleanup may return an error from the failed 
	# disconnects; for now I'll consider this successful 
	# if all the modules have unloaded.
	if ! cleanup ; then
	    lsmod | grep -q portals && return 1
        fi
	return 0
}
run_test 4 "force cleanup ost, then cleanup"

test_5() {
	setup
	touch $DIR/$tfile || return 86
	stop_mds ${FORCE} || return 98

	# cleanup may return an error from the failed 
	# disconnects; for now I'll consider this successful 
	# if all the modules have unloaded.
	if ! cleanup ; then
	    lsmod | grep -q portals && return 1
        fi
	return 0
}
run_test 5 "force cleanup mds, then cleanup"

test_6() {
	setup
	manual_umount_client
	mount_client ${MOUNT} || return 87
	touch $DIR/a || return 86
	cleanup 
}
run_test 6 "manual umount, then mount again"

test_7() {
	setup
	manual_umount_client
	cleanup 
}
run_test 7 "manual umount, then cleanup"

test_8() {
	start_ost
	start_mds

	mount_client $MOUNT  
	mount_client $MOUNT2 

	check_mount2 || return 45
	umount $MOUNT
	umount_client $MOUNT2  
	
	stop_mds
	stop_ost
}
run_test 8 "double mount setup"


equals_msg "Done"
