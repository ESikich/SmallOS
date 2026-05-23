#!/usr/bin/env python3
import sys


def omf_index_size(byte):
    return 2 if byte & 0x80 else 1


def extract_ledata(path):
    data = open(path, "rb").read()
    image = bytearray(64000)
    seen = 0
    pos = 0

    while pos + 3 <= len(data):
        record_type = data[pos]
        record_len = data[pos + 1] | (data[pos + 2] << 8)
        record = data[pos + 3:pos + 3 + record_len]
        pos += 3 + record_len
        if len(record) != record_len:
            break
        if record_type not in (0xA0, 0xA1):
            continue

        payload = record[:-1]
        index_len = omf_index_size(payload[0])
        offset = payload[index_len] | (payload[index_len + 1] << 8)
        chunk = payload[index_len + 2:]
        image[offset:offset + len(chunk)] = chunk
        seen += len(chunk)

    if seen != 64000:
        raise SystemExit("expected 64000 bytes of SIGNON data, got %u" % seen)
    return image


def write_header(path, image):
    with open(path, "w") as out:
        out.write("#ifndef SMALLOS_WOLF3D_SIGNON_H\n")
        out.write("#define SMALLOS_WOLF3D_SIGNON_H\n\n")
        out.write("static const unsigned char smallos_wolf3d_signon[64000] = {\n")
        for offset in range(0, len(image), 16):
            out.write("    ")
            out.write(", ".join(str(byte) for byte in image[offset:offset + 16]))
            out.write(",\n")
        out.write("};\n\n#endif\n")


def main(argv):
    if len(argv) != 3:
        raise SystemExit("usage: extract_wolf3d_signon.py SIGNON.OBJ OUT.h")
    write_header(argv[2], extract_ledata(argv[1]))


if __name__ == "__main__":
    main(sys.argv)
