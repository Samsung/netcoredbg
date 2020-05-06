#!/usr/bin/env python
import re, sys, os

'''
Example of using script:
    1) Put vscode debug console output and run `./get-vscodecmd.py vscode.out > cmd.out`
    2) Run `dos2unix cmd.out`
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