#!/usr/bin/env python3
import csv
import io
import os
import sys
import zipfile
import urllib.request

# URLs
CITIES_URL = "https://download.geonames.org/export/dump/cities15000.zip"
COUNTRY_URL = "https://download.geonames.org/export/dump/countryInfo.txt"
ADMIN1_URL = "https://download.geonames.org/export/dump/admin1CodesASCII.txt"

def download_text(url):
    print(f"Fetching {url}...")
    with urllib.request.urlopen(url) as response:
        return response.read().decode('utf-8')

def load_countries():
    data = download_text(COUNTRY_URL)
    countries = {}
    for line in data.splitlines():
        if line.startswith("#") or not line.strip(): continue
        parts = line.split("\t")
        if len(parts) > 4:
            countries[parts[0]] = parts[4] # ISO -> Name
    return countries

def load_admin1():
    data = download_text(ADMIN1_URL)
    admins = {}
    for line in data.splitlines():
        parts = line.split("\t")
        if len(parts) > 1:
            # Key "US.CO" -> "Colorado"
            admins[parts[0]] = parts[1]
    return admins

def format_pop(n_str):
    try:
        n = int(n_str)
    except:
        return "0"
    if n >= 1000000:
        val = n / 1000000.0
        return f"{val:.1f}M".replace(".0M", "M")
    if n >= 1000:
        val = n / 1000.0
        return f"{val:.0f}K"
    return str(n)

def process_cities(infile, outfile):
    countries = load_countries()
    admins = load_admin1()
    
    cities = []
    
    print(f"Processing {infile}...")
    with open(infile, "r", encoding="utf-8") as f:
        reader = csv.reader(f, delimiter="\t")
        for row in reader:
            # Schema: 0:id 1:name 2:ascii 4:lat 5:lon 8:cc 10:adm1 14:pop
            if len(row) < 15: continue
            
            try:
                name = row[2] # Use ASCII name for compatibility
                if not name: name = row[1]
                
                lat = float(row[4])
                lon = float(row[5])
                cc = row[8]
                admin1 = row[10]
                pop = row[14]
                
                cname = countries.get(cc, cc)
                
                # Admin lookup key: CC.Admin1
                key = f"{cc}.{admin1}"
                aname = admins.get(key, admin1)
                
                # Construct description
                # If admin is numeric or same as country, skip it?
                # HamClock style: "City, Region, Country. Pop X"
                
                parts = [name]
                if aname and aname != name and aname != cname:
                    parts.append(aname)
                parts.append(cname)
                
                desc_base = ", ".join(parts)
                desc = f"{desc_base}. Pop {format_pop(pop)}"
                
                cities.append((lat, lon, desc))
            except ValueError:
                continue

    # HamClock cities.cpp expects roughly N cities?
    # No limit, but memory is limited.
    # cities15000 is ~26k.
    # We might want to clamp this if it causes memory issues.
    
    print(f"Writing {len(cities)} cities to {outfile}...")
    with open(outfile, "w", encoding="utf-8") as f:
        for lat, lon, desc in cities:
            f.write(f'{lat:.4f}, {lon:.4f}, "{desc}"\n')

if __name__ == "__main__":
    if not os.path.exists("cities15000.txt"):
        print("Downloading cities15000.zip...")
        with urllib.request.urlopen(CITIES_URL) as r:
            z = zipfile.ZipFile(io.BytesIO(r.read()))
            z.extract("cities15000.txt")
            
    # Ensure data dir exists
    if not os.path.exists("data"):
        os.makedirs("data")
        
    process_cities("cities15000.txt", "data/cities2.txt")
