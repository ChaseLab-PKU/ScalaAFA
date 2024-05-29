#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/vhost/common.sh"
source "$testdir/common.sh"

function create_device() {
	local pfid=${1:-1}
	local vfid=${2:-0}

	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "CreateDevice",
			"params": {
				"nvme": {
					"physical_id": "$pfid",
					"virtual_id": "$vfid"
				}
			}
		}
	EOF
}

function delete_device() {
	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "DeleteDevice",
			"params": {
				"handle": "$1"
			}
		}
	EOF
}

function attach_volume() {
	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "AttachVolume",
			"params": {
				"device_handle": "$1",
				"volume": {
					"volume_id": "$(uuid2base64 $2)"
				}
			}
		}
	EOF
}

function detach_volume() {
	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "DetachVolume",
			"params": {
				"device_handle": "$1",
				"volume_id": "$(uuid2base64 $2)"
			}
		}
	EOF
}

function vm_count_nvme() {
	vm_exec $1 "grep -l SPDK /sys/class/nvme/*/model" | wc -l
}

function vm_check_subsys_volume() {
	local vm_id=$1
	local nqn=$2
	local uuid=$3

	nvme="$(vm_exec $vm_id "grep -l $nqn /sys/class/nvme/*/subsysnqn" | awk -F/ '{print $5}')"
	if [[ -z "$nvme" ]]; then
		error "FAILED no NVMe on vm=$vm_id with nqn=$nqn"
		return 1
	fi

	tmpuuid="$(vm_exec $vm_id "grep -l $uuid /sys/class/nvme/$nvme/nvme*/uuid")"
	if [[ -z "$tmpuuid" ]]; then
		return 1
	fi
}

function vm_check_subsys_nqn() {
	sleep 1
	nqn=$(vm_exec $1 "grep -l $2 /sys/class/nvme/*/subsysnqn")
	if [[ -z "$nqn" ]]; then
		error "FAILED no NVMe on vm=$1 with nqn=$2"
		return 1
	fi
}

function cleanup() {
	vm_kill_all
	killprocess $tgtpid
	killprocess $smapid
	if [ -e "${VFO_ROOT_PATH}" ]; then rm -rf "${VFO_ROOT_PATH}"; fi
}

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

# SSH VM Password
VM_PASSWORD=root
vm_no=0

VFO_ROOT_PATH="/tmp/sma/vfio-user/qemu"

if [ -e "${VFO_ROOT_PATH}" ]; then rm -rf "${VFO_ROOT_PATH}"; fi
mkdir -p "${VFO_ROOT_PATH}"

# Cleanup old VM:
used_vms=$vm_no
vm_kill_all

vm_setup --os="$VM_IMAGE" --disk-type=virtio --force=$vm_no --qemu-args="-qmp tcp:localhost:10005,server,nowait -device pci-bridge,chassis_nr=1,id=pci.spdk.0"

# Run pre-configured VM and wait for them to start
vm_run $vm_no
vm_wait_for_boot 300 $vm_no

# Start SPDK
$rootdir/build/bin/spdk_tgt &
tgtpid=$!
waitforlisten $tgtpid

# Prepare the target
rpc_cmd bdev_null_create null0 100 4096
rpc_cmd bdev_null_create null1 100 4096

# Start SMA server
$rootdir/scripts/sma.py -c <(
	cat <<- EOF
		devices:
		  - name: 'vfiouser'
		    params:
		      buses:
		        - name: 'pci.spdk.0'
		          count: 32
		      qmp_addr: 127.0.0.1
		      qmp_port: 10005
	EOF
) &
smapid=$!

# Wait until the SMA starts listening
sma_waitforlisten

# Make sure a TCP transport has been created
rpc_cmd nvmf_get_transports --trtype VFIOUSER

# Make sure no nvme subsystems are present
[[ $(vm_exec ${vm_no} nvme list-subsys -o json | jq -r '.Subsystems | length') -eq 0 ]]

# Create a couple of devices and verify them via RPC and SSH
device0=$(create_device 0 0 | jq -r '.handle')
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0
vm_check_subsys_nqn $vm_no nqn.2016-06.io.spdk:vfiouser-0

# Check that there are two subsystems (1 created above + discovery)
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 2 ]]

device1=$(create_device 1 0 | jq -r '.handle')
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1
[[ "$device0" != "$device1" ]]
vm_check_subsys_nqn $vm_no nqn.2016-06.io.spdk:vfiouser-1

# Check that there are three subsystems (2 created above + discovery)
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 3 ]]

# Verify the method is idempotent and sending the same gRPCs won't create new
# devices and will return the same IDs
tmp0=$(create_device 0 0 | jq -r '.handle')
tmp1=$(create_device 1 0 | jq -r '.handle')

[[ $(vm_count_nvme ${vm_no}) -eq 2 ]]

[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 3 ]]
[[ "$tmp0" == "$device0" ]]
[[ "$tmp1" == "$device1" ]]

# Now remove them verifying via RPC
delete_device "$device0"
NOT rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 2 ]]
[[ $(vm_count_nvme ${vm_no}) -eq 1 ]]

delete_device "$device1"
NOT rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0
NOT rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 1 ]]
[[ $(vm_count_nvme ${vm_no}) -eq 0 ]]

# Finally check that removing a non-existing device is also sucessful
delete_device "$device0"
delete_device "$device1"

# Check volume attach/detach
device0=$(create_device 0 0 | jq -r '.handle')
device1=$(create_device 1 0 | jq -r '.handle')
uuid0=$(rpc_cmd bdev_get_bdevs -b null0 | jq -r '.[].uuid')
uuid1=$(rpc_cmd bdev_get_bdevs -b null1 | jq -r '.[].uuid')

# Attach the first volume to a first subsystem
attach_volume "$device0" "$uuid0"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].uuid') == "$uuid0" ]]
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid0

attach_volume "$device1" "$uuid1"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].uuid') == "$uuid0" ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces[0].uuid') == "$uuid1" ]]
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid1

# Attach the same device again and see that it won't fail
attach_volume "$device0" "$uuid0"
attach_volume "$device1" "$uuid1"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].uuid') == "$uuid0" ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces[0].uuid') == "$uuid1" ]]
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid0
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid1
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid1
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid0

# Cross detach volumes and verify they not fail and have not been removed from the subsystems
detach_volume "$device0" "$uuid1"
detach_volume "$device1" "$uuid0"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].uuid') == "$uuid0" ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces[0].uuid') == "$uuid1" ]]
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid0
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid1
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid1
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid0

# Detach volumes and verify they have been removed from the subsystems
detach_volume "$device0" "$uuid0"
detach_volume "$device1" "$uuid1"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 0 ]]
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid0
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid1

# Detach volumes once again and verify they will not fail
detach_volume "$device0" "$uuid0"
detach_volume "$device1" "$uuid1"
detach_volume "$device0" "$uuid1"
detach_volume "$device1" "$uuid0"

delete_device "$device0"
delete_device "$device1"

cleanup
trap - SIGINT SIGTERM EXIT
