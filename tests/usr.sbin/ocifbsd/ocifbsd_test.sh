#!/usr/local/bin/cbsd jexec jname=${jname}
# SPDX-License-Identifier: BSD-2-Clause
#-
# Test script for ocifbsd
# These tests are designed to run in a jail environment
#

set -e

# Test configuration
BUNDLE_DIR="/tmp/ocifbsd_test_bundle"
CONFIG_FILE="$BUNDLE_DIR/config.json"
ROOTFS_DIR="$BUNDLE_DIR/rootfs"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

# Counters
PASSED=0
FAILED=0

# Setup function
atf_test_case setup cleanup
setup_head()
{
	echo "Setup: Create test bundle"
}
setup_body()
{
	# Create bundle directory
	mkdir -p "$BUNDLE_DIR"
	mkdir -p "$ROOTFS_DIR"
	
	# Create minimal rootfs
	mkdir -p "$ROOTFS_DIR/bin"
	echo '#!/bin/sh
echo "Hello from test container"
exit 0' > "$ROOTFS_DIR/bin/sh"
	chmod +x "$ROOTFS_DIR/bin/sh"
	
	# Create minimal OCI config
	cat > "$CONFIG_FILE" << 'EOFCONFIG'
{
	"ociVersion": "1.0.2",
	"root": {
		"path": "/tmp/ocifbsd_test_bundle/rootfs",
		"readonly": false
	},
	"process": {
		"terminal": false,
		"cwd": "/",
		"args": ["/bin/sh", "-c", "echo Hello from test container; exit 0"],
		"env": ["PATH=/bin"]
	},
	"hostname": "test-container",
	"mounts": []
}
EOFCONFIG

	atf_pass
}
setup_cleanup()
{
	rm -rf "$BUNDLE_DIR"
}

# Test: ocifbsd version
atf_test_case test_version
test_version_head()
{
	echo "Test: ocifbsd version command"
}
test_version_body()
{
	ocifbsd -V 2>&1 | grep -q "ocifbsd version"
	atf_pass
}

# Test: ocifbsd help
atf_test_case test_help
test_help_head()
{
	echo "Test: ocifbsd help command"
}
test_help_body()
{
	ocifbsd -h 2>&1 | grep -q "Usage:"
	atf_pass
}

# Test: ocifbsd list (empty)
atf_test_case test_list_empty
test_list_empty_head()
{
	echo "Test: ocifbsd list with no containers"
}
test_list_empty_body()
{
	# Should list nothing and exit cleanly
	ocifbsd list 2>&1 | grep -q "CONTAINER ID"
	atf_pass
}

# Test: ocifbsd state (non-existent)
atf_test_case test_state_nonexistent
test_state_nonexistent_head()
{
	echo "Test: ocifbsd state for non-existent container"
}
test_state_nonexistent_body()
{
	# Should fail gracefully
	! ocifbsd state nonexitent12345 2>&1 | grep -q "not found"
	atf_pass
}

# Test: ocifbsd inspect (non-existent)
atf_test_case test_inspect_nonexistent
test_inspect_nonexistent_head()
{
	echo "Test: ocifbsd inspect for non-existent container"
}
test_inspect_nonexistent_body()
{
	# Should fail gracefully
	! ocifbsd inspect nonexitent12345 2>&1 | grep -q "not found"
	atf_pass
}

# Test: ocifbsd create
atf_test_case test_create
test_create_head()
{
	echo "Test: ocifbsd create"
}
test_create_body()
{
	CONTAINER_ID=$(ocifbsd create "$BUNDLE_DIR" 2>&1)
	if [ -z "$CONTAINER_ID" ]; then
		echo "Failed to create container"
		exit 1
	fi
	
	# Container ID should be 64 hex characters (SHA-256)
	if ! echo "$CONTAINER_ID" | grep -qE '^[a-f0-9]{64}$'; then
		echo "Invalid container ID format: $CONTAINER_ID"
		exit 1
	fi
	
	# Container should be in state directory
	if [ ! -f "/var/run/ocifbsd/${CONTAINER_ID}.json" ]; then
		echo "Container state file not created"
		exit 1
	fi
	
	atf_pass
}

# Test: ocifbsd state (created)
atf_test_create()
{
	# This test depends on test_create running first
}
test_state_created_head()
{
	echo "Test: ocifbsd state for created container"
}
test_state_created_body()
{
	# Create a container first
	CONTAINER_ID=$(ocifbsd create "$BUNDLE_DIR" 2>&1)
	
	# Check state
	STATE=$(ocifbsd state "$CONTAINER_ID" 2>&1)
	if ! echo "$STATE" | grep -q "created"; then
		echo "Expected state 'created', got: $STATE"
		exit 1
	fi
	
	atf_pass
}

# Test: ocifbsd delete
atf_test_case test_delete
test_delete_head()
{
	echo "Test: ocifbsd delete"
}
test_delete_body()
{
	# Create a container first
	CONTAINER_ID=$(ocifbsd create "$BUNDLE_DIR" 2>&1)
	
	# Delete the container
	ocifbsd delete "$CONTAINER_ID" 2>&1
	
	# State file should be removed
	if [ -f "/var/run/ocifbsd/${CONTAINER_ID}.json" ]; then
		echo "Container state file not removed"
		exit 1
	fi
	
	atf_pass
}

# Test: ocifbsd create --name
atf_test_case test_create_with_name
test_create_with_name_head()
{
	echo "Test: ocifbsd create with name"
}
test_create_with_name_body()
{
	# Create with name
	ocifbsd create "$BUNDLE_DIR" --name "test-container" 2>&1
	
	# List should show the container
	if ! ocifbsd list 2>&1 | grep -q "test-container"; then
		echo "Named container not found in list"
		exit 1
	fi
	
	atf_pass
}

# Test: ocifbsd kill (non-existent)
atf_test_case test_kill_nonexistent
test_kill_nonexistent_head()
{
	echo "Test: ocifbsd kill for non-existent container"
}
test_kill_nonexistent_body()
{
	# Should fail gracefully
	! ocifbsd kill nonexitent12345 2>&1 | grep -q "not found"
	atf_pass
}

# Test: ocifbsd delete (non-existent)
atf_test_case test_delete_nonexistent
test_delete_nonexistent_head()
{
	echo "Test: ocifbsd delete for non-existent container"
}
test_delete_nonexistent_body()
{
	# Should fail gracefully
	! ocifbsd delete nonexitent12345 2>&1 | grep -q "not found"
	atf_pass
}

# Main test runner
atf_init_test_cases()
{
	atf_add_test_case setup
	atf_add_test_case test_version
	atf_add_test_case test_help
	atf_add_test_case test_list_empty
	atf_add_test_case test_state_nonexistent
	atf_add_test_case test_inspect_nonexistent
	atf_add_test_case test_create
	atf_add_test_case test_state_created
	atf_add_test_case test_delete
	atf_add_test_case test_create_with_name
	atf_add_test_case test_kill_nonexistent
	atf_add_test_case test_delete_nonexistent
}
