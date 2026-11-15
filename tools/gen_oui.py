#!/usr/bin/env python3
"""Refresh the table of Espressif OUIs (the first three bytes of the MAC addresses of every ESP32) embedded in the web page.

    python tools/gen_oui.py                 # downloads the IEEE registry (about 4 MB) and rewrites the table in main/web/index.html and site/install.html
    python tools/gen_oui.py oui.csv         # same, from a copy of the registry already on disk

The registry is https://standards-oui.ieee.org/oui/oui.csv (public). Only the MA-L blocks assigned to "Espressif Inc." are kept: the page shows the vendor
next to the WiFi and Bluetooth MAC addresses (Statistics, Settings, the installer's board check). Other vendors are not in the table, to keep the page small;
an address with the "locally administered" bit set (a random one) is recognised as such.
"""
import csv, io, re, sys, urllib.request

URL = 'https://standards-oui.ieee.org/oui/oui.csv'
FILES = ['main/web/index.html', 'site/install.html']
BEGIN, END = '/* OUI-BEGIN */', '/* OUI-END */'


def main():
    if len(sys.argv) > 1:
        data = open(sys.argv[1], encoding='utf-8', newline='').read()
    else:
        req = urllib.request.Request(URL, headers={'User-Agent': 'esp32-sony-slink gen_oui'})
        data = urllib.request.urlopen(req, timeout=120).read().decode('utf-8')
    ouis = sorted({row[1].upper() for row in csv.reader(io.StringIO(data))
                   if len(row) >= 3 and row[0] == 'MA-L' and row[2].strip().lower() == 'espressif inc.'})
    if not ouis:
        sys.exit('no Espressif entries found: is this the IEEE registry?')
    block = BEGIN + "\nconst OUI_ESP = new Set('" + ' '.join(ouis) + "'.split(' '));   /* " + str(len(ouis)) + " MA-L blocks of Espressif Inc. (IEEE registry), made by tools/gen_oui.py */\n" + END
    for path in FILES:
        s = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in s else '\n'
        pat = re.compile(re.escape(BEGIN) + r'.*?' + re.escape(END), re.S)
        if not pat.search(s):
            print(path + ': no OUI markers, skipped')
            continue
        s = pat.sub(lambda m: block.replace('\n', nl), s, count=1)
        open(path, 'w', encoding='utf-8', newline='').write(s)
        print(path + ': ' + str(len(ouis)) + ' Espressif OUIs')


main()
