#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
bpf_sh="$rootdir/scripts/bpftrace.sh"

bdevperf_rpc_sock=/var/tmp/bdevperf.sock

nvmftestinit

nvmfappstart -m 0x3

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$rootdir/test/bdev/bdevperf/bdevperf -m 0x4 -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 20 -f &> $testdir/try.txt &
bdevperf_pid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; rm -f $testdir/try.txt; killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock

function get_bdev() {
	$rpc_py -s $bdevperf_rpc_sock bdev_get_bdevs | jq -r '.[].name'
}

function get_controller() {
	$rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_controllers | jq -r '.[].name'
}

# Case 1 test ctrlr_loss_timeout_sec time to try reconnecting to a ctrlr before deleting it
# ctrlr_loss_timeout_sec is 10 reconnect_delay_sec is 5
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_set_options -r -1
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -l 10 -o 5

$rootdir/test/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests &
rpc_pid=$!

sleep 1

$rpc_py nvmf_subsystem_remove_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
sleep 5
[[ "$(get_controller)" == "NVMe0" ]]
[[ "$(get_bdev)" == "NVMe0n1" ]]

# wait for the ctrlr_loss_timeout_sec time 10 sec and check bdevs and controller are deleted
sleep 10
[[ "$(get_controller)" == "" ]]
[[ "$(get_bdev)" == "" ]]

wait $rpc_pid

cat $testdir/try.txt
killprocess $bdevperf_pid
rm -f $testdir/try.txt

# Case 2 test fast_io_fail_timeout_sec
# Time to wait until ctrlr is reconnected before failing I/O to ctrlr
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$rootdir/test/bdev/bdevperf/bdevperf -m 0x4 -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 20 -f &> $testdir/try.txt &
bdevperf_pid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; rm -f $testdir/try.txt; killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock

# ctrlr_loss_timeout_sec is 10 fast_io_fail_timeout_sec is 2 -o reconnect_delay_sec is 1
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_set_options -r -1
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT \
	-f ipv4 -n nqn.2016-06.io.spdk:cnode1 -l 10 -u 2 -o 1

$rootdir/test/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests &
rpc_pid=$!

sleep 1
$rpc_py nvmf_subsystem_remove_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# ctrlr should try to reconnect and I/O submitted should be queued until the listener is added back before 5 sec fast_io_fail_timeout_sec
sleep 1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
wait $rpc_pid
cat $testdir/try.txt
cat /dev/null > $testdir/try.txt

# TODO: Check the IO fail if we wait for 5 sec, needs information from bdevperf

$rootdir/test/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests &
rpc_pid=$!
sleep 1
$rpc_py nvmf_subsystem_remove_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
# bdevperf fails to process the I/O fast_io_fail_timeout_sec expires at 2 sec
sleep 5
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
wait $rpc_pid

cat $testdir/try.txt
killprocess $bdevperf_pid
rm -f $testdir/try.txt

# Case 3 test reconnect_delay_sec
# Time to delay a reconnect trial
$rootdir/test/bdev/bdevperf/bdevperf -m 0x4 -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 20 -f &> $testdir/try.txt &
bdevperf_pid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; rm -f $testdir/try.txt; killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock

#start_trace
$bpf_sh $bdevperf_pid $rootdir/scripts/bpf/nvmf_timeout.bt &> $testdir/trace.txt &
dtrace_pid=$!

$rpc_py -s $bdevperf_rpc_sock bdev_nvme_set_options -r -1

# ctrlr_loss_timeout_sec is 10 reconnect_delay_sec is 2
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -l 10 -o 2
$rootdir/test/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests &
rpc_pid=$!
sleep 1
$rpc_py nvmf_subsystem_remove_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

wait $rpc_pid
cat $testdir/trace.txt

# Check the frequency of delay reconnect
if (("$(grep -c "reconnect delay bdev controller NVMe0" < $testdir/trace.txt)" <= 2)); then
	cat $testdir/try.txt
	false
fi

kill $dtrace_pid
rm -f $testdir/trace.txt
cat $testdir/try.txt

killprocess $bdevperf_pid

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT
rm -f $testdir/try.txt

nvmftestfini
