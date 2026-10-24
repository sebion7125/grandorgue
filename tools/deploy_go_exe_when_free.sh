#!/bin/bash
# Wartet, bis GrandOrgue.exe im Code Exchange nicht mehr gesperrt ist (GO läuft),
# und deployt dann den frischen win64-Build. Nur das .exe - die DLLs sind
# unverändert und waeren beim laufenden GO ohnehin "Text file busy".
SRC="/home/vboxuser/grandorgue/build-scripts/for-win64/build/win64/bin/GrandOrgue.exe"
DST="/media/sf_Code_Exchange/GrandOrgue Dev/bin/GrandOrgue.exe"

if [ ! -f "$SRC" ]; then
  echo "FEHLER: kein Build unter $SRC"
  exit 1
fi

for i in $(seq 1 240); do   # 240 x 30s = max 2 Stunden
  waited=$(( (i - 1) * 30 ))
  # Schon aktuell? (z.B. weil ein spaeterer Build es geschafft hat)
  if cmp -s "$SRC" "$DST"; then
    echo "OK: GrandOrgue.exe war bereits aktuell (nach ${waited}s)"
    exit 0
  fi
  # Kopie versuchen; bei laufendem GO schlaegt sie fehl -> stillschweigend warten
  if cp -f "$SRC" "$DST" 2>/dev/null && cmp -s "$SRC" "$DST"; then
    echo "DEPLOYED: GrandOrgue.exe aktualisiert (GO war ${waited}s lang offen)"
    echo "Groesse: $(stat -c%s "$DST") Bytes"
    exit 0
  fi
  sleep 30
done

echo "TIMEOUT: GrandOrgue.exe war 2h durchgehend gesperrt - Deploy nicht erfolgt"
exit 1
