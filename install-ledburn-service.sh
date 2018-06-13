#!/usr/bin/env bash

if [[ -f ledburn.service ]]; then
	echo "Enabling Service..."
	systemctl enable $(pwd)/ledburn.service || exit -1

	echo "Starting Service..."
	systemctl start ledburn.service
	systemctl status ledburn.service
else
	echo "Could not find ledburn.service. Please run make first"
	exit -1
fi
