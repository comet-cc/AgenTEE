#!/bin/bash
set -x
TargetIP="$1"
TargetUser="$2"

ssh -N -L 8008:127.0.0.1:8008 -L 8009:127.0.0.1:8009 -L 8010:127.0.0.1:8010 "$TargetUser"@"$TargetIP"
