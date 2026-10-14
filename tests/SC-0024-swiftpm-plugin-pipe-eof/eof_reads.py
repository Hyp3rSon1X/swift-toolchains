#!/usr/bin/env python3
"""Counts end-of-file reads per pipe in an strace log: the signature of a read source that
keeps firing on a hung-up pipe.

Usage: eof_reads.py <strace log> [max EOF reads allowed on one pipe]
Record the log with: strace -f -qq -yy -e trace=read [-k] -o <log> <command>
  -yy prints `read(28<pipe:[12345]>, ...)`, so every pipe is identified by its inode even when
  file descriptor numbers are reused; -k adds a stack per call, which this script attributes to
  the first frame outside libc, Foundation and libdispatch.

A healthy reader sees one end-of-file read per pipe (two at most). A source that is never
cancelled at EOF reads the same pipe thousands of times. Exits 1 if any pipe has more EOF reads
than the limit (default 50).
"""
import collections
import re
import sys

log, limit = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 50
call = re.compile(r'^(\d+) +read\((\d+)<pipe:\[(\d+)\]>, (.*)$')
resumed = re.compile(r'^(\d+) +<\.\.\. read resumed>(.*)$')
eof = re.compile(r'= 0$')
skip = re.compile(r'/(libc\.so[^(]*|libFoundation\.so|libFoundationEssentials\.so|libdispatch\.so|libswiftCore\.so)\(')

per_pipe = collections.Counter()
by_frame = collections.Counter()
pending = {}          # tid -> pipe inode of an unfinished read
stack_for = None      # pipe inode whose stack lines follow, or None
frame_taken = False
reads = 0

with open(log, errors="replace") as f:
    for line in f:
        if line.startswith(" > "):
            if stack_for is not None and not frame_taken and not skip.search(line):
                m = re.search(r'\(([^)+]+)\+0x', line)
                frame = m.group(1) if m else line.strip()
                by_frame[frame[:150]] += 1
                frame_taken = True
            continue
        stack_for, frame_taken = None, False
        m = call.match(line)
        if m:
            tid, _, inode, rest = m.groups()
            reads += 1
            if rest.endswith("<unfinished ...>"):
                pending[tid] = inode
                continue
            if eof.search(rest):
                per_pipe[inode] += 1
                stack_for = inode
            continue
        m = resumed.match(line)
        if m:
            tid, rest = m.groups()
            inode = pending.pop(tid, None)
            if inode and eof.search(rest):
                per_pipe[inode] += 1
                stack_for = inode

total = sum(per_pipe.values())
worst = per_pipe.most_common(5)
print(f"pipe reads: {reads}; EOF reads on pipes: {total} over {len(per_pipe)} pipes; "
      f"max on one pipe: {worst[0][1] if worst else 0}")
print("busiest pipes (inode: EOF reads): " + ", ".join(f"{i}: {n}" for i, n in worst))
if by_frame:
    print("EOF reads by first frame outside libc/Foundation/libdispatch/swiftCore:")
    for frame, n in by_frame.most_common(8):
        print(f"  {n:7d}  {frame}")
spinning = [i for i, n in per_pipe.items() if n > limit]
print(f"pipes with more than {limit} EOF reads: {len(spinning)}")
sys.exit(1 if spinning else 0)
