# Genera tls_roots.c: certificados raiz de Google Trust Services (DER) para
# verificar HTTPS en la consola. Fuente: el almacen de certificados del
# sistema (Mozilla / ca-certificates); se comprueba la huella SHA-256.
import subprocess, hashlib, sys
ROOTS = [
  ("GTS Root R1", "/etc/ssl/certs/GTS_Root_R1.pem", "d947432abde7b7fa90fc2e6b59101b1280e0e1c7e4e40fa3c6887fff57a7f4cf"),
  ("GTS Root R3", "/etc/ssl/certs/GTS_Root_R3.pem", "34d8a73ee208d9bcdb0d956520934b4e40e69482596e8b6f73c8426b010a6f48"),
  ("GTS Root R4", "/etc/ssl/certs/GTS_Root_R4.pem", "349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d"),
]
out = sys.argv[1]
lines = ["/* tls_roots.c - GENERADO por herramientas/gen_tls_roots.py. No editar a mano.",
         " * Certificados raiz de Google Trust Services (formato DER). Con ellos net.c",
         " * comprueba que los servidores HTTPS (YouTube, Google, Firebase, Drive) son",
         " * de verdad de Google. Validos hasta 2036. */", "", '#include "tls_roots.h"', ""]
entries = []
for i, (name, path, fp) in enumerate(ROOTS):
    der = subprocess.check_output(["openssl", "x509", "-in", path, "-outform", "DER"])
    h = hashlib.sha256(der).hexdigest()
    assert h == fp, (name, h)
    lines.append("/* %s  SHA-256 %s */" % (name, fp))
    lines.append("static const unsigned char k_root%d[%d] = {" % (i, len(der)))
    for j in range(0, len(der), 16):
        lines.append("    " + ", ".join("0x%02X" % b for b in der[j:j+16]) + ",")
    lines.append("};")
    lines.append("")
    entries.append('    { "%s", k_root%d, sizeof(k_root%d) },' % (name, i, i))
lines.append("const TlsRoot g_tlsRoots[] =\n{")
lines += entries
lines.append("};")
lines.append("const int g_tlsRootCount = %d;" % len(ROOTS))
open(out, "w", newline="\n").write("\n".join(lines) + "\n")
print("ok", out)
