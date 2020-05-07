#!/usr/bin/env python
import re, sys, os

'''
Example of using script:
    $ get-vscodecmd.py vscode_output > cmd
    $ unix2dos cmd
    $ netcoredbg --interpreter=vscode --engineLogging=/tmp < cmd
'''

def do(f):
    for line in open(f).readlines():
        r = re.match(r'^-> \(C\) ({.+})', line)
        if r != None:
            cmd = r.group(1)
            print ("Content-Length: {}\n".format(len(cmd)))
            print (cmd)
    pass

if __name__ == "__main__":
    do(sys.argv[1])

