#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
[ "$#" = 1 ] || { echo 'usage: check-ipa.sh USERSPACE_BUILD'; exit 2; }
work=$1
export LD_LIBRARY_PATH="$work/stage/usr/lib"
verify=$work/libcamera/build/src/apps/ipa-verify/ipa_verify
ipa=$work/stage/usr/lib/libcamera/ipa/ipa_softisp.so
"$verify" "$ipa"
[ ! -e "$work/ipa-tampered-test" ]; mkdir "$work/ipa-tampered-test"
cp "$ipa" "$work/ipa-tampered-test/ipa_softisp.so"
cp "$ipa.sign" "$work/ipa-tampered-test/ipa_softisp.so.sign"
printf 'tamper' >> "$work/ipa-tampered-test/ipa_softisp.so"
if "$verify" "$work/ipa-tampered-test/ipa_softisp.so"; then
 echo 'ERROR: tampered IPA unexpectedly accepted'; exit 1
fi
echo 'PASS valid IPA accepted; tampered IPA rejected'
