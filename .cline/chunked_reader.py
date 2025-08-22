#!/usr/bin/env python3
"""
.chunked_reader.py

Kleines Hilfsprogramm zum schonenden Einlesen großer Textdateien in Chunks.
Zweck:
 - Ermöglicht das sequenzielle Lesen großer Dateien ohne komplettes Einlesen in den Speicher.
 - Ausgabe ist newline-delimited JSON (NDJSON) pro Chunk:
     { "path": "...", "chunk_index": 0, "offset": 0, "size": 2048, "text": "..." }
 - Kann auf der Kommandozeile in Pipes verwendet werden oder von anderen Tools aufgerufen werden.

Usage:
  python3 .cline/chunked_reader.py --file docs/GO_Project_Chats.md --chunk-size 2048
  python3 .cline/chunked_reader.py --file docs/GO_Project_Chats.md --chunk-size 2048 --max-chunks 10

Optionen:
  --file/-f        Pfad zur Eingabedatei (erforderlich)
  --chunk-size/-s  Chunk-Größe in Bytes (Default: 2048)
  --max-chunks/-m  Max. Anzahl Chunks (optional)
  --encoding/-e    Text-Encoding (Default: utf-8)
  --ndjson         Ausgabe als NDJSON (default: true). Wenn nicht gesetzt, werden Chunks als plain text mit Separatoren ausgegeben.
  --show-offsets   Gib zusätzlich Offsets aus (nützlich für Byte-Range-Lesezugriff)

Hinweis:
 - Dieses Skript ist bewusst klein gehalten und erzeugt lesbare Chunks.
 - Für binäre Dateien oder proprietäre Formate sollte stattdessen ein spezialisierter Parser genutzt werden.
"""
from __future__ import annotations
import argparse
import json
import os
import sys

def iter_chunks(path: str, chunk_size: int = 2048, encoding: str = "utf-8"):
    """
    Generator: yield (index, offset, bytes_data)
    Liest die Datei in Binärmodus und liefert rohe Bytes. Decoding erfolgt in caller.
    """
    index = 0
    offset = 0
    with open(path, "rb") as f:
        while True:
            data = f.read(chunk_size)
            if not data:
                break
            yield index, offset, data
            offset += len(data)
            index += 1

def main():
    parser = argparse.ArgumentParser(description="Chunked file reader for large text files.")
    parser.add_argument("--file", "-f", required=True, help="Pfad zur Eingabedatei")
    parser.add_argument("--chunk-size", "-s", type=int, default=2048, help="Chunk-Größe in Bytes (Default: 2048)")
    parser.add_argument("--max-chunks", "-m", type=int, default=0, help="Maximale Anzahl Chunks (0 = unbegrenzt)")
    parser.add_argument("--encoding", "-e", default="utf-8", help="Text-Encoding für Dekodierung (Default: utf-8)")
    parser.add_argument("--ndjson", action="store_true", default=True, help="Ausgabe als NDJSON pro Chunk (default: True)")
    parser.add_argument("--no-ndjson", dest="ndjson", action="store_false", help="Ausgabe als reiner Text mit Separator")
    parser.add_argument("--show-offsets", action="store_true", help="Gibt Offset-Informationen aus (Teil der JSON/Output)")
    args = parser.parse_args()

    path = args.file
    if not os.path.isfile(path):
        print(json.dumps({"error": "file-not-found", "path": path}), file=sys.stderr)
        sys.exit(2)

    count = 0
    try:
        for idx, offset, data in iter_chunks(path, args.chunk_size, args.encoding):
            if args.max_chunks and idx >= args.max_chunks:
                break
            try:
                text = data.decode(args.encoding, errors="replace")
            except Exception:
                text = data.decode(args.encoding, errors="replace")
            if args.ndjson:
                out = {
                    "path": path,
                    "chunk_index": idx,
                    "offset": offset if args.show_offsets else None,
                    "size": len(data),
                    "text": text
                }
                # Entferne None-Felder für sauberere Ausgabe
                if out["offset"] is None:
                    del out["offset"]
                print(json.dumps(out, ensure_ascii=False))
            else:
                # Trenne Chunks klar erkennbar
                sep = "\n---CHUNK-{}-END---\n".format(idx)
                sys.stdout.write(text)
                sys.stdout.write(sep)
            count += 1
    except BrokenPipeError:
        # Ermöglicht Pipe/Head-Abbruch ohne Tracebacks
        try:
            sys.stdout.close()
        except Exception:
            pass
        sys.exit(0)

    # Optional: Zusammenfassung in stderr
    print(json.dumps({"status":"done","file": path, "chunks_sent": count}), file=sys.stderr)

if __name__ == "__main__":
    main()
