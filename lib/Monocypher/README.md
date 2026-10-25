# Monocypher

The four source files in `src/` come from Monocypher 4.0.2:
https://github.com/LoupVaillant/Monocypher/tree/4.0.2/src

`monocypher-ed25519.c` provides RFC 8032 Ed25519 with SHA-512. The source
headers carry the BSD-2-Clause OR CC0-1.0 license notice. The firmware uses
only signature verification. Host tests exercise the RFC 8032 empty-message
vector and the server/guest shared release vector.
