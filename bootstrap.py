#!/usr/bin/env python

# This script simply downloads waf to the current directory

from __future__ import print_function
import os, sys, stat, hashlib, subprocess

WAFRELEASE = "waf-1.8.1"
WAFURL     = "http://ftp.waf.io/pub/release/" + WAFRELEASE
SHA256HASH = "ec658116ba0b96629d91fde0b32321849e866e0819f1e835c4c2c7f7ffe1a21d"

if os.path.exists("waf"):
    wafver = subprocess.check_output(['./waf', '--version']).decode()
    if WAFRELEASE.split('-')[1] == wafver.split(' ')[1]:
        print("Found 'waf', skipping download.")
        sys.exit(0)

try:
    from urllib.request import urlopen
except:
    from urllib2 import urlopen

print("Downloading %s..." % WAFURL)
waf = urlopen(WAFURL).read()

if SHA256HASH == hashlib.sha256(waf).hexdigest():
    with open("waf", "wb") as wf:
        wf.write(waf)

    os.chmod("waf", os.stat("waf").st_mode | stat.S_IXUSR)
    print("Checksum verified.")
else:
    print("The checksum of the downloaded file does not match!")
    print(" - got:      {}".format(hashlib.sha256(waf).hexdigest()))
    print(" - expected: {}".format(SHA256HASH))
    print("Please download and verify the file manually.")

    sys.exit(1)
