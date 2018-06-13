#!/usr/bin/env bash

cd $(dirname $0)

if [[ -f ledburn.service ]]; then
	echo "Stopping Service..."
	systemctl stop ledburn.service

	echo "Disabling Service..."
	systemctl disable $(pwd)/ledburn.service || exit -1
else
	echo "Could not find ledburn.service. Please run make first"
	exit -1
fi
