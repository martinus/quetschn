#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# zcomp-split.sh <linux tree>: applies Sergey Senozhatsky's series "zram: redesign zcomp and rework
# backends" of 5th October 2026 (v1, 10 patches) to a git tree with git am. Patch 09 splits zram's
# contexts into one for compression and one for decompression, which port.py and tools/zram-vm/ follow.
# It applies to mm-unstable fb0fbeb37 of akpm/mm.git, not to KERNEL_COMMIT of CI. The patches come from
# the thread's mbox on lore.kernel.org, each checked by its git patch-id, so replies in the thread don't
# matter. Until the series is in mainline (#103), this is how a tree with the split is made:
#
#   git clone --depth 1 --branch mm-unstable https://git.kernel.org/pub/scm/linux/kernel/git/akpm/mm.git
#   tools/kernel-port/zcomp-split.sh mm
set -euo pipefail
[[ $# -eq 1 ]] || { sed -n '4,12p' "$0" >&2; exit 2; }
tree=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# patch number and git patch-id --stable of v1
ids=(
    01 9566a4e52c9130132981990adeaa833a6f5004ce
    02 1b4e2c5ff1895293b545697666af1adb97f96a4c
    03 a7b41f1d86bd56892a5632519b9b2dadf75cc34b
    04 4ee8b802130e29de14d1999fbfaa6c071e0ab1f0
    05 011ab4e49f35de716dd7e9ff7e9fa4c566b2f75e
    06 661fa5b01e2dcad7218dd3358f590b44983ee424
    07 b5bc6e6a92b8bb55ebc417c423f6695b328c2aac
    08 462b47afe46dbeba1dad35c250be9b7066c5fa3e
    09 5cd8272a578697aa4db7c5804b3a6f242b4f7ff6
    10 74adbdaf008589d1f0e7b15cae6d06c9961e2e8d
)
curl -sSfL -o "$work/thread.mbox.gz" \
    "https://lore.kernel.org/all/20261005122036.718976-10-senozhatsky@chromium.org/t.mbox.gz"
gunzip "$work/thread.mbox.gz"
mkdir "$work/split"
git mailsplit -o"$work/split" "$work/thread.mbox" >/dev/null
for f in "$work"/split/*; do
    n=$(grep -a -m1 '^Subject: \[PATCH [0-9][0-9]/10\]' "$f" | sed 's/.*PATCH \([0-9][0-9]\)\/10.*/\1/' || true)
    [[ -n $n ]] && cp "$f" "$work/$n.patch"
done
for ((k = 0; k < ${#ids[@]}; k += 2)); do
    n=${ids[k]} want=${ids[k + 1]}
    [[ -f $work/$n.patch ]] || { echo "patch $n/10 is not in the thread" >&2; exit 1; }
    got=$(git patch-id --stable <"$work/$n.patch" | cut -d' ' -f1)
    [[ $got == "$want" ]] || { echo "patch $n/10 is not the one of v1: patch-id $got" >&2; exit 1; }
done
for ((k = 0; k < ${#ids[@]}; k += 2)); do
    git -C "$tree" -c user.name=zcomp-split -c user.email=zcomp-split@localhost am -q "$work/${ids[k]}.patch"
done
git -C "$tree" log --oneline -10
