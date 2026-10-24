#!/bin/bash
# Baut clbench und misst den Solve-Kernel ueber die interessanten
# Ordnungen.  AUF DEM HOST ausfuehren, nicht in der VM: gebaut werden
# muss mit dem Compiler und dem ICD-Loader der Maschine, auf der es dann
# laeuft - der Ablageort der Quellen ist egal, ein sshfs-Mount tut es.
# Seit 06.08.2026 geht es auch in der VM: pocl ist installiert, dann
# rechnet der "Device"-Pfad auf der CPU - fuer VERIFIKATION vollwertig,
# nur die Zeiten sind dann keine GPU-Zeiten.
#
#   bash tools/concealment/run_clbench.sh [bins] [ordnungen...]
#
# bins = 2049 * Kanalzahl; Vorgabe 12294 (5.1).
set -e
cd "$(dirname "$0")/../.."

BINS="${1:-12294}"
shift || true
ORDERS=("$@")
[ ${#ORDERS[@]} -eq 0 ] && ORDERS=(2 4 6 8 12)

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

for need in gcc python3; do
  command -v "$need" >/dev/null || { echo "FEHLT: $need"; exit 1; }
done
if [ ! -f /usr/include/CL/cl.h ]; then
  echo "FEHLT: OpenCL-Header - sudo apt install opencl-headers ocl-icd-opencl-dev"
  exit 1
fi

echo "Referenz aus src/xorcist/xorcist.cpp erzeugen..."
python3 tools/concealment/make_ref.py > "$OUT/ref.c"
echo "clbench bauen..."
gcc -O2 -o "$OUT/clbench" tools/concealment/clbench.c "$OUT/ref.c" \
  -lOpenCL -lm

if command -v clinfo >/dev/null; then
  echo
  clinfo 2>/dev/null | grep -E "^  Platform Name|^  Device Name" | head -8
fi
echo
echo "Bins: $BINS   Ordnungen: ${ORDERS[*]}   Genauigkeit: ${XO_FP32:+fp32}${XO_FP32:-fp64}"
echo "Die maximale Abweichung MUSS 0.00e+00 sein - alles andere ist ein Befund."
echo
for o in "${ORDERS[@]}"; do
  "$OUT/clbench" "$o" "$BINS" src/xorcist/xorcist_kernels.cl | tee -a "$OUT/times.txt"
done
echo
echo "EINORDNUNG"
echo "  Der Test loest ALLE $BINS Bins in einem Rutsch.  Die Engine loest"
echo "  pro Periode nur 1/16 davon: die Koeffizienten kommen aus einem"
echo "  ~20-Frame-Gedaechtnis und werden rollierend erneuert (Zyklus ="
echo "  AR_SOLVE_EVERY 4 Rasterpunkte x arStride 4 Perioden = 16), damit"
echo "  die Last flach liegt statt als Schwall alle 16 Perioden."
echo "  Jeder Bin ist dadurch genauso frisch wie vorher, nur verteilt."
echo
printf "  %-9s %14s %16s %12s\n" Ordnung "alle Bins" "pro Periode" "von 2667 us"
for o in "${ORDERS[@]}"; do
  us=$(grep -h "Ordnung *$o," "$OUT/times.txt" 2>/dev/null | tail -1 \
       | sed -E "s/.*Device *([0-9.]+) us.*/\1/")
  [ -z "$us" ] && continue
  awk -v o="$o" -v t="$us" "BEGIN{p=t/16; printf \"  %-9s %11.1f us %13.1f us %11.1f %%\\n\", o, t, p, 100*p/2667}"
done
echo
echo "  NICHT enthalten: Kernel-Start (auf NVIDIA 5-20 us, und pro Periode"
echo "  braeuchte es drei davon) sowie die Transfers.  Bei kleinen Ordnungen"
echo "  ist der Start also teurer als die Rechnung - die GPU lohnt ab etwa"
echo "  Ordnung 6 bis 8, und genau dort kann die CPU nicht mehr mit."
echo
echo "GENAUIGKEIT nach Stufen (gemessen, nicht geraten):"
echo "  XO_FP32_STORE=1  Normalgleichungen in float ..... 0.0000 grad"
echo "  XO_FP32_SOLVE=1  Gauss-Elimination in float ..... 0.0000 grad"
echo "  XO_FP32_ROOT=1   Durand-Kerner in float ......... 174.6 grad (!)"
echo "  Empfohlen also:  XO_FP32_STORE=1 XO_FP32_SOLVE=1  (Wurzeln double)"
echo
echo "ALTE NOTIZ: der Kernel rechnet per Vorgabe in DOUBLE, und eine"
echo "  GeForce faehrt FP64 mit 1/64 der FP32-Rate (3070: 0.317 gegen"
echo "  20.3 TFLOPS).  Die obigen Zeiten sind also NICHT das, was die"
echo "  Karte kann.  Gegenprobe:"
echo "      XO_FP32=1 bash tools/concealment/run_clbench.sh $BINS ${ORDERS[*]}"
echo "  Aber Vorsicht mit dem Ergebnis: gegen die double-Referenz weichen"
echo "  die Koeffizienten bei Ordnung 4 um 1.6e-07 ab, bei Ordnung 8 schon"
echo "  um 1.5e-01.  Was das in dB auf echtem Material kostet, ist offen -"
echo "  die Wurzelkappung dahinter faengt vielleicht einen Teil davon ab."
