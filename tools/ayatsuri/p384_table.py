"""Generate experiment-only TF-PSA sources from the ESP-IDF 6.0.1 inputs."""
import hashlib
from pathlib import Path
import sys


def pinned_source(path, sha256):
    text = path.read_text(encoding="utf-8")
    if hashlib.sha256(text.encode()).hexdigest() != sha256:
        raise ValueError(f"unexpected IDF 6.0.1 source: {path}")
    return text


def substitute(text, original, replacement):
    if text.count(original) != 1:
        raise ValueError(f"expected one matching source section: {original}")
    return text.replace(original, replacement)


def write_source(path, text):
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_bytes(text.encode("utf-8"))


def generate(src, dest):
    ecp = pinned_source(src / "ecp.c", "7cddf4ad1e74a9a1cb213957af6ce12639141aee864af049eb27c1b25f706b0d")
    curves = pinned_source(src / "ecp_curves.c", "4ee173ab25b2404fb77e43adadb3eadfa30e82e339bfab0e011313312778e6af")
    # Expose only the unmodified upstream P-384 width-six, 32-point table.
    # All the other curve tables retain their stock preprocessor conditions.
    curves = substitute(curves,
                        "#if MBEDTLS_ECP_FIXED_POINT_OPTIM == 1\n"
                        "static const mbedtls_mpi_uint secp384r1_T_0_X[]",
                        "#if 1 /* P-384 public verification experiment */\n"
                        "static const mbedtls_mpi_uint secp384r1_T_0_X[]")
    # Static tables are immutable, library-owned flash data. Keep the library's
    # static-table ownership rule and width selection for this curve only.
    ecp = substitute(ecp,
                     "    (void) grp;\n    return 0;\n#endif\n}\n\n/*\n * Unallocate",
                     "    return grp->id == MBEDTLS_ECP_DP_SECP384R1 &&\n"
                     "           grp->T != NULL && grp->T_size == 0;\n"
                     "#endif\n}\n\n/*\n * Unallocate")
    # ecdsa_verify passes the group generator by its exact address. The
    # scoped wrapper marks its private group only for the candidate arm.
    ecp = substitute(ecp, "    p_eq_g = 0;\n#endif\n\n    /* Pick window size",
                     "    p_eq_g = P == &grp->G && grp->t_data == grp &&\n"
                     "             ecp_group_is_static_comb_table(grp);\n"
                     "#endif\n\n    /* Pick window size")
    ecp = substitute(ecp, "        T = grp->T;\n        T_ok = 1;",
                     "        T = grp->T;\n        T_ok = 1;\n"
                     "        if (grp->t_data == grp) grp->t_data = &grp->G;")
    dest.mkdir(parents=True, exist_ok=True)
    write_source(dest / "ecp.c", ecp)
    write_source(dest / "ecp_curves.c", curves)


if __name__ == "__main__":
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
