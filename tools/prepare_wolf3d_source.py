#!/usr/bin/env python3
import argparse
import pathlib
import re
import shutil


ACTORAT_TILEMAP_CHAIN_ASSIGN = re.compile(
    rb"\(unsigned\)(actorat\[[^\]]+\]\[[^\]]+\])\s*=\s*"
    rb"(tilemap\[[^\]]+\]\[[^\]]+\])\s*=\s*([^;]+);"
)
ACTORAT_CAST_ASSIGN = re.compile(
    rb"\(unsigned\)(actorat\[[^\]]+\]\[[^\]]+\])\s*=(?!=)\s*([^;]+);"
)
CAST_ADDRESS = re.compile(
    rb"&\((memptr|void[ \t]+_seg[ \t]*\*)\)([A-Za-z_][A-Za-z0-9_\[\] +\-]*)"
)
DOUBLE_CAST_ADDRESS = re.compile(
    rb"&\(\((memptr|void[ \t]+_seg[ \t]*\*)\)([A-Za-z_][A-Za-z0-9_\[\] +\-]*)\)"
)
SIMPLE_ASM_LINE = re.compile(rb"(?m)^[ \t]*asm[ \t]+[^\n]*\n")
ASM_MACRO_BLOCK = re.compile(
    rb"(?ms)^[ \t]*#define[ \t]+([A-Za-z_][A-Za-z0-9_]*(?:\([^)]*\))?)[ \t]+asm\{.*?;\}[ \t]*\n"
)
ASM_MACRO_LINE = re.compile(rb"(?m)^[ \t]*#define[ \t]+([A-Za-z_][A-Za-z0-9_]*(?:\([^)]*\))?)[ \t]+asm\{[^\n]*\}\n")
XMS_CHECK_FUNC = re.compile(
    rb"boolean[ \t]+MML_CheckForXMS[ \t]*\(void\)[ \t\r\n]*\{.*?\n\}",
    re.S,
)
XMS_SETUP_FUNC = re.compile(
    rb"void[ \t]+MML_SetupXMS[ \t]*\(void\)[ \t\r\n]*\{.*?\ndone:;\n\}",
    re.S,
)
CA_FARREAD_FUNC = re.compile(
    rb"boolean[ \t]+CA_FarRead[ \t]*"
    rb"\(int[ \t]+handle,[ \t]*byte[ \t]+far[ \t]*\*dest,[ \t]*long[ \t]+length\)"
    rb"[ \t\r\n]*\{.*?\ndone:[ \t;]*\n\treturn[ \t]+true;\n\}",
    re.S,
)
CA_FARWRITE_FUNC = re.compile(
    rb"boolean[ \t]+CA_FarWrite[ \t]*"
    rb"\(int[ \t]+handle,[ \t]*byte[ \t]+far[ \t]*\*source,[ \t]*long[ \t]+length\)"
    rb"[ \t\r\n]*\{.*?\ndone:[ \t;]*\n\treturn[ \t]+true;\n\}",
    re.S,
)
CAL_HUFFEXPAND_BLOCK = re.compile(
    rb"void[ \t]+CAL_HuffExpand[ \t]*"
    rb"\(byte[ \t]+huge[ \t]*\*source,[ \t]*byte[ \t]+huge[ \t]*\*dest,"
    rb"[ \t\r\n]*long[ \t]+length,[ \t]*huffnode[ \t]*\*hufftable,"
    rb"[ \t]*boolean[ \t]+screenhack\)[ \t\r\n]*\{.*?\n\}"
    rb"(?=[ \t\r\n]*/\*[ \t\r\n=]*CAL_CarmackExpand)",
    re.S,
)
CAL_CARMACKEXPAND_BLOCK = re.compile(
    rb"void[ \t]+CAL_CarmackExpand[ \t]*"
    rb"\(unsigned[ \t]+far[ \t]*\*source,[ \t]*unsigned[ \t]+far[ \t]*\*dest,"
    rb"[ \t]*unsigned[ \t]+length\)[ \t\r\n]*\{.*?\n\}"
    rb"(?=[ \t\r\n]*/\*[ \t\r\n=]*CA_RLEWcompress)",
    re.S,
)
CA_RLEWEXPAND_BLOCK = re.compile(
    rb"void[ \t]+CA_RLEWexpand[ \t]*"
    rb"\(unsigned[ \t]+huge[ \t]*\*source,[ \t]*unsigned[ \t]+huge[ \t]*\*dest,"
    rb"[ \t]*long[ \t]+length,[ \t\r\n]*[ \t]*unsigned[ \t]+rlewtag\)"
    rb"[ \t\r\n]*\{.*?\n\}"
    rb"(?=[ \t\r\n]*/\*[ \t\r\n=]*CACHE MANAGER ROUTINES)",
    re.S,
)
CAL_OPTIMIZE_NODES_FUNC = re.compile(
    rb"void[ \t]+CAL_OptimizeNodes[ \t]*\(huffnode[ \t]+\*table\)"
    rb"[ \t\r\n]*\{.*?\n\}",
    re.S,
)
FIXED_BY_FRAC_FUNC = re.compile(
    rb"fixed[ \t]+FixedByFrac[ \t]*\(fixed[ \t]+a,[ \t]*fixed[ \t]+b\)"
    rb"[ \t\r\n]*\{.*?\n\}[ \t\r\n]*\n#pragma warn \+rvl",
    re.S,
)
CALC_HEIGHT_FUNC = re.compile(
    rb"int[ \t]+CalcHeight[ \t]*\(void\)"
    rb"[ \t\r\n]*\{.*?\n\}[ \t\r\n]*\n//=========================================================================="
    rb"(?=[ \t\r\n]*/\*[ \t\r\n=]*ScalePost)",
    re.S,
)
SCALE_POST_FUNC = re.compile(
    rb"void[ \t]+near[ \t]+ScalePost[ \t]*\(void\)"
    rb"[^\n]*\n[ \t\r\n]*\{.*?\n\}"
    rb"(?=\n\nvoid[ \t]+FarScalePost)",
    re.S,
)
SCALE_LINE_FUNC = re.compile(
    rb"void[ \t]+near[ \t]+ScaleLine[ \t]*\(void\)"
    rb"[^\n]*\n[ \t\r\n]*\{.*?\n\}"
    rb"(?=[ \t\r\n]*/\*)",
    re.S,
)
POSTSOURCE_SET = re.compile(
    rb"(?m)^([ \t]*)\*\( \(\(unsigned \*\)&postsource\)\+1\) = "
    rb"\(unsigned\)([^;]+);\n[ \t]*postsource = \(long\)\(uintptr_t\)texture;"
)
VGA_CLEAR_SCREEN_FUNC = re.compile(
    rb"void[ \t]+VGAClearScreen[ \t]*\(void\)"
    rb"[ \t\r\n]*\{.*?\n\}[ \t\r\n]*\n//=========================================================================="
    rb"(?=[ \t\r\n]*/\*[ \t\r\n=]*CalcRotate)",
    re.S,
)
THREED_REFRESH_FUNC = re.compile(
    rb"void[ \t]+ThreeDRefresh[ \t]*\(void\)"
    rb"[ \t\r\n]*\{.*?\n\}[ \t\r\n]*\n//===========================================================================\n",
    re.S,
)
INL_START_MOUSE_FUNC = re.compile(
    rb"static[ \t]+boolean[ \t\r\n]+INL_StartMouse\(void\)"
    rb"[ \t\r\n]*\{.*?\n\}"
    rb"(?=\n\n///////////////////////////////////////////////////////////////////////////\n//\n//\tINL_ShutMouse)",
    re.S,
)
WL1_DATA_GRAPHICS = [
    "H_BJPIC",
    "H_CASTLEPIC",
    "H_KEYBOARDPIC",
    "H_JOYPIC",
    "H_HEALPIC",
    "H_TREASUREPIC",
    "H_GUNPIC",
    "H_KEYPIC",
    "H_BLAZEPIC",
    "H_WEAPON1234PIC",
    "H_WOLFLOGOPIC",
    "H_VISAPIC",
    "H_MCPIC",
    "H_IDLOGOPIC",
    "H_TOPWINDOWPIC",
    "H_LEFTWINDOWPIC",
    "H_RIGHTWINDOWPIC",
    "H_BOTTOMINFOPIC",
    "H_GALACTIXPIC",
    "C_OPTIONSPIC",
    "C_CURSOR1PIC",
    "C_CURSOR2PIC",
    "C_NOTSELECTEDPIC",
    "C_SELECTEDPIC",
    "C_FXTITLEPIC",
    "C_DIGITITLEPIC",
    "C_MUSICTITLEPIC",
    "C_MOUSELBACKPIC",
    "C_BABYMODEPIC",
    "C_EASYPIC",
    "C_NORMALPIC",
    "C_HARDPIC",
    "C_LOADSAVEDISKPIC",
    "C_DISKLOADING1PIC",
    "C_DISKLOADING2PIC",
    "C_CONTROLPIC",
    "C_CUSTOMIZEPIC",
    "C_LOADGAMEPIC",
    "C_SAVEGAMEPIC",
    "C_EPISODE1PIC",
    "C_EPISODE2PIC",
    "C_EPISODE3PIC",
    "C_EPISODE4PIC",
    "C_EPISODE5PIC",
    "C_EPISODE6PIC",
    "C_CODEPIC",
    "C_TIMECODEPIC",
    "C_LEVELPIC",
    "C_NAMEPIC",
    "C_SCOREPIC",
    "C_JOY1PIC",
    "C_JOY2PIC",
    "L_GUYPIC",
    "L_COLONPIC",
    "L_NUM0PIC",
    "L_NUM1PIC",
    "L_NUM2PIC",
    "L_NUM3PIC",
    "L_NUM4PIC",
    "L_NUM5PIC",
    "L_NUM6PIC",
    "L_NUM7PIC",
    "L_NUM8PIC",
    "L_NUM9PIC",
    "L_PERCENTPIC",
    "L_APIC",
    "L_BPIC",
    "L_CPIC",
    "L_DPIC",
    "L_EPIC",
    "L_FPIC",
    "L_GPIC",
    "L_HPIC",
    "L_IPIC",
    "L_JPIC",
    "L_KPIC",
    "L_LPIC",
    "L_MPIC",
    "L_NPIC",
    "L_OPIC",
    "L_PPIC",
    "L_QPIC",
    "L_RPIC",
    "L_SPIC",
    "L_TPIC",
    "L_UPIC",
    "L_VPIC",
    "L_WPIC",
    "L_XPIC",
    "L_YPIC",
    "L_ZPIC",
    "L_EXPOINTPIC",
    "L_APOSTROPHEPIC",
    "L_GUY2PIC",
    "L_BJWINSPIC",
    "STATUSBARPIC",
    "TITLEPIC",
    "PG13PIC",
    "CREDITSPIC",
    "HIGHSCORESPIC",
    "KNIFEPIC",
    "GUNPIC",
    "MACHINEGUNPIC",
    "GATLINGGUNPIC",
    "NOKEYPIC",
    "GOLDKEYPIC",
    "SILVERKEYPIC",
    "N_BLANKPIC",
    "N_0PIC",
    "N_1PIC",
    "N_2PIC",
    "N_3PIC",
    "N_4PIC",
    "N_5PIC",
    "N_6PIC",
    "N_7PIC",
    "N_8PIC",
    "N_9PIC",
    "FACE1APIC",
    "FACE1BPIC",
    "FACE1CPIC",
    "FACE2APIC",
    "FACE2BPIC",
    "FACE2CPIC",
    "FACE3APIC",
    "FACE3BPIC",
    "FACE3CPIC",
    "FACE4APIC",
    "FACE4BPIC",
    "FACE4CPIC",
    "FACE5APIC",
    "FACE5BPIC",
    "FACE5CPIC",
    "FACE6APIC",
    "FACE6BPIC",
    "FACE6CPIC",
    "FACE7APIC",
    "FACE7BPIC",
    "FACE7CPIC",
    "FACE8APIC",
    "GOTGATLINGPIC",
    "MUTANTBJPIC",
    "PAUSEDPIC",
    "GETPSYCHEDPIC",
]


def build_wl1_data_graphics_header() -> bytes:
    lines = [
        "//////////////////////////////////////",
        "//",
        "// Graphics .H file for staged .WL1 data",
        "// Derived by decoding VGAHEAD/VGAGRAPH/VGADICT.WL1",
        "//",
        "//////////////////////////////////////",
        "",
        "typedef enum {",
    ]
    for index, name in enumerate(WL1_DATA_GRAPHICS, start=3):
        if index == 3:
            lines.append(f"\t\t{name}=3,")
        else:
            lines.append(f"\t\t{name},\t\t\t\t// {index}")
    lines.extend(
        [
            "",
            "\t\tORDERSCREEN=148,",
            "\t\tERRORSCREEN,\t\t\t// 149",
            "\t\tENUMEND",
            "\t     } graphicnums;",
            "",
            "//",
            "// Data LUMPs",
            "//",
            "#define README_LUMP_START\t\t3",
            "#define README_LUMP_END\t\t\t21",
            "",
            "#define CONTROLS_LUMP_START\t\t22",
            "#define CONTROLS_LUMP_END\t\t54",
            "",
            "#define LEVELEND_LUMP_START\t\t55",
            "#define LEVELEND_LUMP_END\t\t97",
            "",
            "#define LATCHPICS_LUMP_START\t\t103",
            "#define LATCHPICS_LUMP_END\t\t146",
            "",
            "",
            "//",
            "// Amount of each data item",
            "//",
            "#define NUMCHUNKS    156",
            "#define NUMFONT      2",
            "#define NUMFONTM     0",
            "#define NUMPICS      144",
            "#define NUMPICM      0",
            "#define NUMSPRITES   0",
            "#define NUMTILE8     0",
            "#define NUMTILE8M    0",
            "#define NUMTILE16    0",
            "#define NUMTILE16M   0",
            "#define NUMTILE32    0",
            "#define NUMTILE32M   0",
            "#define NUMEXTERNS   8",
            "//",
            "// File offsets for data items",
            "//",
            "#define STRUCTPIC    0",
            "",
            "#define STARTFONT    1",
            "#define STARTFONTM   3",
            "#define STARTPICS    3",
            "#define STARTPICM    147",
            "#define STARTSPRITES 147",
            "#define STARTTILE8   147",
            "#define STARTTILE8M  147",
            "#define STARTTILE16  147",
            "#define STARTTILE16M 147",
            "#define STARTTILE32  147",
            "#define STARTTILE32M 147",
            "#define STARTEXTERNS 148",
            "",
            "//",
            "// Thank you for using IGRAB!",
            "//",
            "",
        ]
    )
    return ("\n".join(lines)).encode("ascii")


def normalize_c_source(data: bytes) -> bytes:
    data = data.replace(b"\x1a", b"")
    data = XMS_CHECK_FUNC.sub(
        b"boolean MML_CheckForXMS (void)\n{\n\tnumUMBs = 0;\n\treturn false;\n}",
        data,
    )
    data = XMS_SETUP_FUNC.sub(
        b"void MML_SetupXMS (void)\n{\n}",
        data,
    )
    data = CA_FARREAD_FUNC.sub(
        b"boolean CA_FarRead (int handle, byte far *dest, long length)\n"
        b"{\n"
        b"\tbyte *out = dest;\n"
        b"\twhile (length > 0)\n"
        b"\t{\n"
        b"\t\tlong chunk = length > 32768 ? 32768 : length;\n"
        b"\t\tlong got = read(handle, out, chunk);\n"
        b"\t\tif (got <= 0)\n"
        b"\t\t\treturn false;\n"
        b"\t\tout += got;\n"
        b"\t\tlength -= got;\n"
        b"\t}\n"
        b"\treturn true;\n"
        b"}",
        data,
    )
    data = CA_FARWRITE_FUNC.sub(
        b"boolean CA_FarWrite (int handle, byte far *source, long length)\n"
        b"{\n"
        b"\tbyte *in = source;\n"
        b"\twhile (length > 0)\n"
        b"\t{\n"
        b"\t\tlong chunk = length > 32768 ? 32768 : length;\n"
        b"\t\tlong put = write(handle, in, chunk);\n"
        b"\t\tif (put <= 0)\n"
        b"\t\t\treturn false;\n"
        b"\t\tin += put;\n"
        b"\t\tlength -= put;\n"
        b"\t}\n"
        b"\treturn true;\n"
        b"}",
        data,
    )
    data = CAL_HUFFEXPAND_BLOCK.sub(
        b"void CAL_HuffExpand (byte huge *source, byte huge *dest,\n"
        b"  long length,huffnode *hufftable, boolean screenhack)\n"
        b"{\n"
        b"\thuffnode *headptr = hufftable + 254;\n"
        b"\thuffnode *node = headptr;\n"
        b"\tunsigned mask = 1;\n"
        b"\tbyte inbyte = *source++;\n"
        b"\tlong written = 0;\n"
        b"\tlong plane_length = screenhack ? length / 4 : length;\n"
        b"\tlong plane_offset = 0;\n"
        b"\tunsigned plane = 0;\n"
        b"\n"
        b"\twhile (written < length)\n"
        b"\t{\n"
        b"\t\tunsigned code = (inbyte & mask) ? node->bit1 : node->bit0;\n"
        b"\t\tmask <<= 1;\n"
        b"\t\tif (mask == 256)\n"
        b"\t\t{\n"
        b"\t\t\tinbyte = *source++;\n"
        b"\t\t\tmask = 1;\n"
        b"\t\t}\n"
        b"\t\tif (code >= 256)\n"
        b"\t\t{\n"
        b"\t\t\tuintptr_t table_start = (uintptr_t)hufftable;\n"
        b"\t\t\tuintptr_t table_end = table_start + 255 * sizeof(*hufftable);\n"
        b"\t\t\tuintptr_t codeptr = (uintptr_t)code;\n"
        b"\t\t\tif (codeptr >= table_start && codeptr < table_end)\n"
        b"\t\t\t\tnode = (huffnode *)codeptr;\n"
        b"\t\t\telse if (code < 511)\n"
        b"\t\t\t\tnode = hufftable + (code - 256);\n"
        b"\t\t\telse\n"
        b"\t\t\t\tQuit(\"CAL_HuffExpand: bad huffman node\");\n"
        b"\t\t\tcontinue;\n"
        b"\t\t}\n"
        b"\t\tif (screenhack)\n"
        b"\t\t{\n"
        b"\t\t\twolf3d_vga_write_planar_byte(dest, (unsigned)plane_offset, plane, (byte)code);\n"
        b"\t\t\tplane_offset++;\n"
        b"\t\t\tif (plane_offset >= plane_length)\n"
        b"\t\t\t{\n"
        b"\t\t\t\tplane_offset = 0;\n"
        b"\t\t\t\tplane++;\n"
        b"\t\t\t}\n"
        b"\t\t}\n"
        b"\t\telse\n"
        b"\t\t\tdest[written] = (byte)code;\n"
        b"\t\twritten++;\n"
        b"\t\tnode = headptr;\n"
        b"\t}\n"
        b"}",
        data,
    )
    data = CAL_CARMACKEXPAND_BLOCK.sub(
        b"void CAL_CarmackExpand (uint16_t far *source, uint16_t far *dest, unsigned length)\n"
        b"{\n"
        b"\tuint16_t ch,chhigh,count,offset;\n"
        b"\tuint16_t far *copyptr, far *inptr, far *outptr;\n"
        b"\n"
        b"\tlength /= 2;\n"
        b"\tinptr = source;\n"
        b"\toutptr = dest;\n"
        b"\n"
        b"\twhile (length)\n"
        b"\t{\n"
        b"\t\tch = *inptr++;\n"
        b"\t\tchhigh = ch >> 8;\n"
        b"\t\tif (chhigh == NEARTAG)\n"
        b"\t\t{\n"
        b"\t\t\tcount = ch & 0xff;\n"
        b"\t\t\tif (!count)\n"
        b"\t\t\t{\n"
        b"\t\t\t\tch |= *(byte *)inptr;\n"
        b"\t\t\t\tinptr = (uint16_t *)((byte *)inptr + 1);\n"
        b"\t\t\t\t*outptr++ = ch;\n"
        b"\t\t\t\tlength--;\n"
        b"\t\t\t}\n"
        b"\t\t\telse\n"
        b"\t\t\t{\n"
        b"\t\t\t\toffset = *(byte *)inptr;\n"
        b"\t\t\t\tinptr = (uint16_t *)((byte *)inptr + 1);\n"
        b"\t\t\t\tcopyptr = outptr - offset;\n"
        b"\t\t\t\tlength -= count;\n"
        b"\t\t\t\twhile (count--)\n"
        b"\t\t\t\t\t*outptr++ = *copyptr++;\n"
        b"\t\t\t}\n"
        b"\t\t}\n"
        b"\t\telse if (chhigh == FARTAG)\n"
        b"\t\t{\n"
        b"\t\t\tcount = ch & 0xff;\n"
        b"\t\t\tif (!count)\n"
        b"\t\t\t{\n"
        b"\t\t\t\tch |= *(byte *)inptr;\n"
        b"\t\t\t\tinptr = (uint16_t *)((byte *)inptr + 1);\n"
        b"\t\t\t\t*outptr++ = ch;\n"
        b"\t\t\t\tlength--;\n"
        b"\t\t\t}\n"
        b"\t\t\telse\n"
        b"\t\t\t{\n"
        b"\t\t\t\toffset = *inptr++;\n"
        b"\t\t\t\tcopyptr = dest + offset;\n"
        b"\t\t\t\tlength -= count;\n"
        b"\t\t\t\twhile (count--)\n"
        b"\t\t\t\t\t*outptr++ = *copyptr++;\n"
        b"\t\t\t}\n"
        b"\t\t}\n"
        b"\t\telse\n"
        b"\t\t{\n"
        b"\t\t\t*outptr++ = ch;\n"
        b"\t\t\tlength--;\n"
        b"\t\t}\n"
        b"\t}\n"
        b"}",
        data,
    )
    data = CA_RLEWEXPAND_BLOCK.sub(
        b"void CA_RLEWexpand (uint16_t huge *source, uint16_t huge *dest,long length,\n"
        b"  uint16_t rlewtag)\n"
        b"{\n"
        b"\tuint16_t value,count;\n"
        b"\tuint16_t huge *end = dest + length / 2;\n"
        b"\n"
        b"\twhile (dest < end)\n"
        b"\t{\n"
        b"\t\tvalue = *source++;\n"
        b"\t\tif (value != rlewtag)\n"
        b"\t\t\t*dest++ = value;\n"
        b"\t\telse\n"
        b"\t\t{\n"
        b"\t\t\tcount = *source++;\n"
        b"\t\t\tvalue = *source++;\n"
        b"\t\t\twhile (count-- && dest < end)\n"
        b"\t\t\t\t*dest++ = value;\n"
        b"\t\t}\n"
        b"\t}\n"
        b"}",
        data,
    )
    data = CAL_OPTIMIZE_NODES_FUNC.sub(
        b"void CAL_OptimizeNodes (huffnode *table)\n"
        b"{\n"
        b"\t(void)table;\n"
        b"}",
        data,
    )
    data = FIXED_BY_FRAC_FUNC.sub(
        b"fixed FixedByFrac (fixed a, fixed b)\n"
        b"{\n"
        b"\tuint32_t magnitude = (uint32_t)b;\n"
        b"\tint negative = 0;\n"
        b"\tint64_t result;\n"
        b"\n"
        b"\tif (magnitude & 0x80000000u)\n"
        b"\t{\n"
        b"\t\tnegative = !negative;\n"
        b"\t\tmagnitude &= 0x7fffffffu;\n"
        b"\t}\n"
        b"\tif (a < 0)\n"
        b"\t{\n"
        b"\t\tnegative = !negative;\n"
        b"\t\ta = -a;\n"
        b"\t}\n"
        b"\tresult = ((int64_t)a * (int64_t)magnitude) >> 16;\n"
        b"\treturn (fixed)(negative ? -result : result);\n"
        b"}\n\n"
        b"#pragma warn +rvl",
        data,
    )
    data = data.replace(
        b"\tasm\tmov\tax,[WORD PTR heightnumerator]\n"
        b"\tasm\tmov\tdx,[WORD PTR heightnumerator+2]\n"
        b"\tasm\tidiv\t[WORD PTR nx+1]\t\t\t// nx>>8\n"
        b"\tasm\tmov\t[WORD PTR temp],ax\n"
        b"\tasm\tmov\t[WORD PTR temp+2],dx",
        b"\t{\n"
        b"\t\tlong divisor = nx >> 8;\n"
        b"\t\tif (divisor < 1)\n"
        b"\t\t\tdivisor = 1;\n"
        b"\t\ttemp = heightnumerator / divisor;\n"
        b"\t}",
    )
    data = CALC_HEIGHT_FUNC.sub(
        b"int CalcHeight (void)\n"
        b"{\n"
        b"\tfixed gxt,gyt,nx;\n"
        b"\tlong gx,gy,divisor;\n"
        b"\n"
        b"\tgx = xintercept-viewx;\n"
        b"\tgxt = FixedByFrac(gx,viewcos);\n"
        b"\tgy = yintercept-viewy;\n"
        b"\tgyt = FixedByFrac(gy,viewsin);\n"
        b"\tnx = gxt-gyt;\n"
        b"\tif (nx<mindist)\n"
        b"\t\tnx=mindist;\n"
        b"\tdivisor = nx >> 8;\n"
        b"\tif (divisor < 1)\n"
        b"\t\tdivisor = 1;\n"
        b"\treturn (int)(heightnumerator / divisor);\n"
        b"}\n\n"
        b"//==========================================================================",
        data,
    )
    data = VGA_CLEAR_SCREEN_FUNC.sub(
        b"void VGAClearScreen (void)\n"
        b"{\n"
        b"\twolf3d_vga_clear_view();\n"
        b"}\n\n"
        b"//==========================================================================",
        data,
    )
    data = THREED_REFRESH_FUNC.sub(
        b"void ThreeDRefresh (void)\n"
        b"{\n"
        b"\tmemset(spotvis,0,sizeof(spotvis));\n"
        b"\tbufferofs += screenofs;\n"
        b"\tVGAClearScreen();\n"
        b"\tWallRefresh();\n"
        b"\tDrawScaleds();\n"
        b"\tDrawPlayerWeapon();\n"
        b"\tif (fizzlein)\n"
        b"\t{\n"
        b"\t\tfizzlein = false;\n"
        b"\t\tlasttimecount = TimeCount = 0;\n"
        b"\t}\n"
        b"\tbufferofs -= screenofs;\n"
        b"\tdisplayofs = bufferofs;\n"
        b"\tVW_UpdateScreen();\n"
        b"\tbufferofs += SCREENSIZE;\n"
        b"\tif (bufferofs > PAGE3START)\n"
        b"\t\tbufferofs = PAGE1START;\n"
        b"\tframeon++;\n"
        b"\tPM_NextFrame();\n"
        b"}\n\n"
        b"//===========================================================================\n",
        data,
    )
    data = INL_START_MOUSE_FUNC.sub(
        b"static boolean\n"
        b"INL_StartMouse(void)\n"
        b"{\n"
        b"\tMouse(MReset);\n"
        b"\treturn _AX == 0xffff;\n"
        b"}",
        data,
    )
    data = data.replace(b"typedef\tunsigned\tint\t\t\tword;",
                        b"typedef\tuint16_t\t\t\tword;")
    data = data.replace(b"typedef\tunsigned\tlong\t\tlongword;",
                        b"typedef\tuint32_t\t\tlongword;")
    data = data.replace(
        b"\tif (!*error)\n"
        b"\t{\n",
        b"\tif (!error || !*error)\n"
        b"\t{\n",
    )
    data = data.replace(
        b"IN_ClearKeysDown(void)\n"
        b"{\n"
        b"\tint\ti;\n"
        b"\n"
        b"\tLastScan = sc_None;\n"
        b"\tLastASCII = key_None;\n"
        b"\tmemset (Keyboard,0,sizeof(Keyboard));\n"
        b"}",
        b"IN_ClearKeysDown(void)\n"
        b"{\n"
        b"\tint\ti;\n"
        b"\t(void)i;\n"
        b"\twolf3d_input_clear();\n"
        b"}",
    )
    data = data.replace(
        b"\tINL_ShutKbd();\n"
        b"\n"
        b"\tIN_Started = false;\n",
        b"\tINL_ShutKbd();\n"
        b"\twolf3d_input_clear();\n"
        b"\n"
        b"\tIN_Started = false;\n",
    )
    data = data.replace(
        b"register\tKeyboardDef\t*def;\n"
        b"\n"
        b"\tdx = dy = 0;",
        b"register\tKeyboardDef\t*def;\n"
        b"\n"
        b"\twolf3d_input_poll();\n"
        b"\tdx = dy = 0;",
    )
    data = data.replace(
        b"\twhile (!(result = LastScan))\n"
        b"\t\t;\n"
        b"\tLastScan = 0;",
        b"\twhile (!(result = LastScan))\n"
        b"\t{\n"
        b"\t\twolf3d_input_poll();\n"
        b"\t\tVL_WaitVBL(1);\n"
        b"\t}\n"
        b"\tLastScan = 0;",
    )
    data = data.replace(
        b"\twhile (!(result = LastASCII))\n"
        b"\t\t;\n"
        b"\tLastASCII = '\\0';",
        b"\twhile (!(result = LastASCII))\n"
        b"\t{\n"
        b"\t\twolf3d_input_poll();\n"
        b"\t\tVL_WaitVBL(1);\n"
        b"\t}\n"
        b"\tLastASCII = '\\0';",
    )
    data = data.replace(
        b"boolean IN_CheckAck (void)\n"
        b"{\n"
        b"\tunsigned\ti,buttons;\n"
        b"\n"
        b"//\n",
        b"boolean IN_CheckAck (void)\n"
        b"{\n"
        b"\tunsigned\ti,buttons;\n"
        b"\n"
        b"\twolf3d_input_poll();\n"
        b"//\n",
    )
    data = data.replace(
        b"\tdo\n"
        b"\t{\n"
        b"\t\tif (IN_CheckAck())\n"
        b"\t\t\treturn true;\n"
        b"\t} while (TimeCount - lasttime < delay);",
        b"\tdo\n"
        b"\t{\n"
        b"\t\tif (IN_CheckAck())\n"
        b"\t\t\treturn true;\n"
        b"\t\tVL_WaitVBL(1);\n"
        b"\t} while (TimeCount - lasttime < delay);",
    )
    data = data.replace(
        b"\twhile (!IN_CheckAck ())\n"
        b"\t;",
        b"\twhile (!IN_CheckAck ())\n"
        b"\t\tVL_WaitVBL(1);",
    )
    data = data.replace(
        b"\twhile (!IN_CheckAck () && TimeCount < 700);",
        b"\twhile (!IN_CheckAck () && TimeCount < 700)\n"
        b"\t\tVL_WaitVBL(1);",
    )
    data = data.replace(
        b"\twhile(!IN_CheckAck())\n"
        b"\t  BJ_Breathe();",
        b"#ifdef SMALLOS_WOLF3D_SOURCE_PROBE\n"
        b"\t{\n"
        b"\t  extern int wolf3d_probe_skip_level_completed_ack;\n"
        b"\t  if (!wolf3d_probe_skip_level_completed_ack)\n"
        b"\t  {\n"
        b"#endif\n"
        b"\twhile(!IN_CheckAck())\n"
        b"\t{\n"
        b"\t  BJ_Breathe();\n"
        b"\t  VL_WaitVBL(1);\n"
        b"\t}\n"
        b"#ifdef SMALLOS_WOLF3D_SOURCE_PROBE\n"
        b"\t  }\n"
        b"\t}\n"
        b"#endif",
    )
    data = data.replace(
        b"\t\twhile (TimeCount<lasttimecount+DEMOTICS)\n"
        b"\t\t;",
        b"\t\twhile (TimeCount<lasttimecount+DEMOTICS)\n"
        b"\t\t\tVL_WaitVBL(1);",
    )
    data = data.replace(
        b"\tdo\n"
        b"\t{\n"
        b"\t\tnewtime = TimeCount;\n"
        b"\t\ttics = newtime-lasttimecount;\n"
        b"\t} while (!tics);\t\t\t// make sure at least one tic passes",
        b"\tdo\n"
        b"\t{\n"
        b"\t\tnewtime = TimeCount;\n"
        b"\t\ttics = newtime-lasttimecount;\n"
        b"\t\tif (!tics)\n"
        b"\t\t\tVL_WaitVBL(1);\n"
        b"\t} while (!tics);\t\t\t// make sure at least one tic passes",
    )
    data = data.replace(
        b"\tlong\tnewtime,oldtimecount;\n"
        b"\n"
        b"//\n"
        b"// calculate tics since last refresh for adaptive timing\n"
        b"//\n",
        b"\tlong\tnewtime,oldtimecount;\n"
        b"\n"
        b"\twolf3d_sync_time_count();\n"
        b"\n"
        b"//\n"
        b"// calculate tics since last refresh for adaptive timing\n"
        b"//\n",
    )
    data = data.replace(
        b"\tbyte\tbuttonbits;\n"
        b"\n"
        b"//\n"
        b"// get timing info for last frame\n"
        b"//\n",
        b"\tbyte\tbuttonbits;\n"
        b"\n"
        b"\twolf3d_sync_time_count();\n"
        b"\twolf3d_input_poll();\n"
        b"\n"
        b"//\n"
        b"// get timing info for last frame\n"
        b"//\n",
    )
    data = data.replace(
        b"\tmemset (buttonstate,0,sizeof(buttonstate));\n"
        b"\n"
        b"\tif (demoplayback)\n",
        b"\tmemset (buttonstate,0,sizeof(buttonstate));\n"
        b"\twolf3d_input_poll();\n"
        b"\n"
        b"\tif (demoplayback)\n",
    )
    data = data.replace(
        b"\tReadConfig ();\n"
        b"\n\n"
        b"//\n"
        b"// HOLDING DOWN 'M' KEY?\n"
        b"//\n",
        b"\tReadConfig ();\n"
        b"\twolf3d_input_poll();\n"
        b"\n\n"
        b"//\n"
        b"// HOLDING DOWN 'M' KEY?\n"
        b"//\n",
    )
    data = data.replace(
        b"\t\tread(file,Scores,sizeof(HighScore) * MaxScores);\n"
        b"\n"
        b"\t\tread(file,&sd,sizeof(sd));\n"
        b"\t\tread(file,&sm,sizeof(sm));\n"
        b"\t\tread(file,&sds,sizeof(sds));\n"
        b"\n"
        b"\t\tread(file,&mouseenabled,sizeof(mouseenabled));\n"
        b"\t\tread(file,&joystickenabled,sizeof(joystickenabled));\n"
        b"\t\tread(file,&joypadenabled,sizeof(joypadenabled));\n"
        b"\t\tread(file,&joystickprogressive,sizeof(joystickprogressive));\n"
        b"\t\tread(file,&joystickport,sizeof(joystickport));\n"
        b"\n"
        b"\t\tread(file,&dirscan,sizeof(dirscan));\n"
        b"\t\tread(file,&buttonscan,sizeof(buttonscan));\n"
        b"\t\tread(file,&buttonmouse,sizeof(buttonmouse));\n"
        b"\t\tread(file,&buttonjoy,sizeof(buttonjoy));\n"
        b"\n"
        b"\t\tread(file,&viewsize,sizeof(viewsize));\n"
        b"\t\tread(file,&mouseadjustment,sizeof(mouseadjustment));",
        b"\t\twolf3d_read_config_file(file,Scores,MaxScores,&sd,&sm,&sds);",
    )
    data = data.replace(
        b"\t\twrite(file,Scores,sizeof(HighScore) * MaxScores);\n"
        b"\n"
        b"\t\twrite(file,&SoundMode,sizeof(SoundMode));\n"
        b"\t\twrite(file,&MusicMode,sizeof(MusicMode));\n"
        b"\t\twrite(file,&DigiMode,sizeof(DigiMode));\n"
        b"\n"
        b"\t\twrite(file,&mouseenabled,sizeof(mouseenabled));\n"
        b"\t\twrite(file,&joystickenabled,sizeof(joystickenabled));\n"
        b"\t\twrite(file,&joypadenabled,sizeof(joypadenabled));\n"
        b"\t\twrite(file,&joystickprogressive,sizeof(joystickprogressive));\n"
        b"\t\twrite(file,&joystickport,sizeof(joystickport));\n"
        b"\n"
        b"\t\twrite(file,&dirscan,sizeof(dirscan));\n"
        b"\t\twrite(file,&buttonscan,sizeof(buttonscan));\n"
        b"\t\twrite(file,&buttonmouse,sizeof(buttonmouse));\n"
        b"\t\twrite(file,&buttonjoy,sizeof(buttonjoy));\n"
        b"\n"
        b"\t\twrite(file,&viewsize,sizeof(viewsize));\n"
        b"\t\twrite(file,&mouseadjustment,sizeof(mouseadjustment));",
        b"\t\twolf3d_write_config_file(file,Scores,MaxScores,SoundMode,MusicMode,DigiMode);",
    )
    data = data.replace(
        b"\tfile = open(configname,O_CREAT | O_BINARY | O_WRONLY,\n",
        b"\tfile = open(configname,O_CREAT | O_BINARY | O_WRONLY | O_TRUNC,\n",
    )
    data = data.replace(
        b"\t\tclose(file);\n"
        b"\n"
        b"\t\tif (sd == sdm_AdLib && !AdLibPresent && !SoundBlasterPresent)\n",
        b"\t\tclose(file);\n"
        b"\t\twolf3d_validate_control_config();\n"
        b"\n"
        b"\t\tif (sd == sdm_AdLib && !AdLibPresent && !SoundBlasterPresent)\n",
    )
    data = data.replace(
        b"\tDrawMainMenu();\n"
        b"\tMenuFadeIn();\n"
        b"\tStartGame=0;",
        b"\tDrawMainMenu();\n"
        b"\tMenuFadeIn();\n"
        b"\tWaitKeyUp();\n"
        b"#ifdef SMALLOS_WOLF3D_SOURCE_PROBE\n"
        b"\t{\n"
        b"\t\textern int wolf3d_probe_stop_after_control_panel_frame;\n"
        b"\t\tif (wolf3d_probe_stop_after_control_panel_frame)\n"
        b"\t\t{\n"
        b"\t\t\tCleanupControlPanel();\n"
        b"#ifdef SPEAR\n"
        b"\t\t\tUnCacheLump (OPTIONS_LUMP_START,OPTIONS_LUMP_END);\n"
        b"#endif\n"
        b"\t\t\treturn;\n"
        b"\t\t}\n"
        b"\t}\n"
        b"#endif\n"
        b"\tStartGame=0;",
    )
    data = data.replace(
        b"\twhile (Keyboard[sc_I] || Keyboard[sc_D]);",
        b"\twhile (Keyboard[sc_I] || Keyboard[sc_D])\n"
        b"\t{\n"
        b"\t\twolf3d_input_poll();\n"
        b"\t\tVL_WaitVBL(1);\n"
        b"\t}",
    )
    data = data.replace(
        b"\t\t\t\t\twhile(Keyboard[sc_LeftArrow]);",
        b"\t\t\t\t\twhile(Keyboard[sc_LeftArrow])\n"
        b"\t\t\t\t\t{\n"
        b"\t\t\t\t\t\twolf3d_input_poll();\n"
        b"\t\t\t\t\t\tVL_WaitVBL(1);\n"
        b"\t\t\t\t\t}",
    )
    data = data.replace(
        b"\t\t\t\t\twhile(Keyboard[sc_RightArrow]);",
        b"\t\t\t\t\twhile(Keyboard[sc_RightArrow])\n"
        b"\t\t\t\t\t{\n"
        b"\t\t\t\t\t\twolf3d_input_poll();\n"
        b"\t\t\t\t\t\tVL_WaitVBL(1);\n"
        b"\t\t\t\t\t}",
    )
    data = data.replace(
        b"\twhile(TimeCount<8);",
        b"\twhile(TimeCount<8)\n"
        b"\t\tVL_WaitVBL(1);",
    )
    data = data.replace(
        b"\twhile(ReadAnyControl(&ci),\tci.button0|\n"
        b"\t\t\t\t\t\t\t\tci.button1|\n"
        b"\t\t\t\t\t\t\t\tci.button2|\n"
        b"\t\t\t\t\t\t\t\tci.button3|\n"
        b"\t\t\t\t\t\t\t\tKeyboard[sc_Space]|\n"
        b"\t\t\t\t\t\t\t\tKeyboard[sc_Enter]|\n"
        b"\t\t\t\t\t\t\t\tKeyboard[sc_Escape]);",
        b"\twhile(ReadAnyControl(&ci),\tci.button0|\n"
        b"\t\t\t\t\t\t\t\tci.button1|\n"
        b"\t\t\t\t\t\t\t\tci.button2|\n"
        b"\t\t\t\t\t\t\t\tci.button3|\n"
        b"\t\t\t\t\t\t\t\tKeyboard[sc_Space]|\n"
        b"\t\t\t\t\t\t\t\tKeyboard[sc_Enter]|\n"
        b"\t\t\t\t\t\t\t\tKeyboard[sc_Escape])\n"
        b"\t{\n"
        b"\t\tVL_WaitVBL(1);\n"
        b"\t}",
    )
    data = data.replace(
        b"\t\t}\n"
        b"\t}\n"
        b"}\n\n\n"
        b"////////////////////////////////////////////////////////////////////\n"
        b"//\n"
        b"// DRAW DIALOG AND CONFIRM YES OR NO TO QUESTION",
        b"\t\t}\n"
        b"\t}\n"
        b"\tVL_WaitVBL(1);\n"
        b"}\n\n\n"
        b"////////////////////////////////////////////////////////////////////\n"
        b"//\n"
        b"// DRAW DIALOG AND CONFIRM YES OR NO TO QUESTION",
    )
    data = data.replace(
        b"\t\tThreeDRefresh ();\n"
        b"\n"
        b"\t\t//\n"
        b"\t\t// MAKE FUNNY FACE IF BJ DOESN'T MOVE FOR AWHILE",
        b"\t\tThreeDRefresh ();\n"
        b"#ifdef SMALLOS_WOLF3D_SOURCE_PROBE\n"
        b"\t\t{\n"
        b"\t\t\textern int wolf3d_probe_stop_after_first_game_frame;\n"
        b"\t\t\tif (wolf3d_probe_stop_after_first_game_frame)\n"
        b"\t\t\t{\n"
        b"\t\t\t\tplaystate = ex_abort;\n"
        b"\t\t\t\treturn;\n"
        b"\t\t\t}\n"
        b"\t\t}\n"
        b"#endif\n"
        b"\n"
        b"\t\t//\n"
        b"\t\t// MAKE FUNNY FACE IF BJ DOESN'T MOVE FOR AWHILE",
    )
    data = data.replace(
        b"startplayloop:\n"
        b"\t\tPlayLoop ();\n"
        b"\n"
        b"#ifdef SPEAR",
        b"startplayloop:\n"
        b"\t\tPlayLoop ();\n"
        b"#ifdef SMALLOS_WOLF3D_SOURCE_PROBE\n"
        b"\t\t{\n"
        b"\t\t\textern int wolf3d_probe_stop_after_first_game_frame;\n"
        b"\t\t\tif (wolf3d_probe_stop_after_first_game_frame)\n"
        b"\t\t\t{\n"
        b"\t\t\t\tStopMusic ();\n"
        b"\t\t\t\tingame = false;\n"
        b"\t\t\t\treturn;\n"
        b"\t\t\t}\n"
        b"\t\t}\n"
        b"#endif\n"
        b"\n"
        b"#ifdef SPEAR",
    )
    data = data.replace(
        b"\tbufferofs -= screenofs;\n"
        b"\tdisplayofs = bufferofs;\n"
        b"\n\n"
        b"\tbufferofs += SCREENSIZE;",
        b"\tbufferofs -= screenofs;\n"
        b"\tdisplayofs = bufferofs;\n"
        b"\tVW_UpdateScreen();\n"
        b"\n\n"
        b"\tbufferofs += SCREENSIZE;",
    )
    data = data.replace(
        b"\t#ifdef SPANISH\n"
        b"\t} while(!Keyboard[sc_S] && !Keyboard[sc_N] && !Keyboard[sc_Escape]);",
        b"\twolf3d_input_poll();\n"
        b"\tVL_WaitVBL(1);\n\n"
        b"\t#ifdef SPANISH\n"
        b"\t} while(!Keyboard[sc_S] && !Keyboard[sc_N] && !Keyboard[sc_Escape]);",
    )
    data = data.replace(
        b"\twhile(Keyboard[sc_S] || Keyboard[sc_N] || Keyboard[sc_Escape]);",
        b"\twhile(Keyboard[sc_S] || Keyboard[sc_N] || Keyboard[sc_Escape])\n"
        b"\t{\n"
        b"\t\twolf3d_input_poll();\n"
        b"\t\tVL_WaitVBL(1);\n"
        b"\t}",
    )
    data = data.replace(
        b"\twhile(Keyboard[sc_Y] || Keyboard[sc_N] || Keyboard[sc_Escape]);",
        b"\twhile(Keyboard[sc_Y] || Keyboard[sc_N] || Keyboard[sc_Escape])\n"
        b"\t{\n"
        b"\t\twolf3d_input_poll();\n"
        b"\t\tVL_WaitVBL(1);\n"
        b"\t}",
    )
    data = SIMPLE_ASM_LINE.sub(b"", data)
    data = SCALE_POST_FUNC.sub(
        b"void near ScalePost (void)\n"
        b"{\n"
        b"\tif (!postwidth)\n"
        b"\t\treturn;\n"
        b"\twolf3d_scale_post(postx, postwidth, wallheight[postx]);\n"
        b"}",
        data,
    )
    data = SCALE_LINE_FUNC.sub(
        b"void near ScaleLine (void)\n"
        b"{\n"
        b"\twolf3d_scale_line();\n"
        b"}",
        data,
    )
    data = data.replace(
        b"\tdisplayofs = bufferofs;\n"
        b"\n\n"
        b"\tbufferofs += SCREENSIZE;",
        b"\tdisplayofs = bufferofs;\n"
        b"\tVW_UpdateScreen();\n"
        b"\n\n"
        b"\tbufferofs += SCREENSIZE;",
    )
    data = ASM_MACRO_BLOCK.sub(rb"#define \1 ((void)0)\n", data)
    data = ASM_MACRO_LINE.sub(rb"#define \1 ((void)0)\n", data)
    data = ACTORAT_TILEMAP_CHAIN_ASSIGN.sub(
        rb"\2 = \3;\n\t\t\1 = (objtype *)(uintptr_t)(\3);",
        data,
    )
    data = ACTORAT_CAST_ASSIGN.sub(
        rb"\1 = (objtype *)(uintptr_t)(\2);",
        data,
    )
    data = data.replace(
        b"length = *((unsigned far *)demoptr)++;\n\tdemoptr++;",
        b"length = *(uint16_t *)demoptr;\n\tdemoptr += 3;",
    )
    data = data.replace(
        b"\t// Read in header variables\n"
        b"\tread(PageFile,&ChunksInFile,sizeof(ChunksInFile));\n"
        b"\tread(PageFile,&PMSpriteStart,sizeof(PMSpriteStart));\n"
        b"\tread(PageFile,&PMSoundStart,sizeof(PMSoundStart));",
        b"\t// Read in header variables\n"
        b"\t{\n"
        b"\t\tuint16_t value16;\n"
        b"\t\tread(PageFile,&value16,sizeof(value16));\n"
        b"\t\tChunksInFile = value16;\n"
        b"\t\tread(PageFile,&value16,sizeof(value16));\n"
        b"\t\tPMSpriteStart = value16;\n"
        b"\t\tread(PageFile,&value16,sizeof(value16));\n"
        b"\t\tPMSoundStart = value16;\n"
        b"\t}",
    )
    data = data.replace(
        b"\tword\t\t\tfar *lengthptr;",
        b"\tuint16_t\t\tfar *lengthptr;",
    )
    data = data.replace(
        b"\tsize = sizeof(word) * ChunksInFile;",
        b"\tsize = sizeof(uint16_t) * ChunksInFile;",
    )
    data = data.replace(
        b"\tlengthptr = (word far *)buf;",
        b"\tlengthptr = (uint16_t far *)buf;",
    )
    data = data.replace(b"} menuitems;", b"};")
    data = DOUBLE_CAST_ADDRESS.sub(rb"(\1 *)&\2", data)
    data = CAST_ADDRESS.sub(rb"(\1 *)&\2", data)
    data = data.replace(
        b"ch |= *((unsigned char far *)inptr)++;",
        b"ch |= *(unsigned char *)inptr;\n\t\t\t\tinptr = (unsigned *)((unsigned char *)inptr + 1);",
    )
    data = data.replace(
        b"offset = *((unsigned char far *)inptr)++;",
        b"offset = *(unsigned char *)inptr;\n\t\t\t\tinptr = (unsigned *)((unsigned char *)inptr + 1);",
    )
    data = data.replace(
        b"(unsigned)postsource = texture;",
        b"postsource = (long)(uintptr_t)texture;",
    )
    data = POSTSOURCE_SET.sub(
        rb"\1wolf3d_set_post_source((const byte far *)(\2), texture);",
        data,
    )
    data = data.replace(
        b"typedef struct\n"
        b"{\n"
        b"\tunsigned\tcodeofs[65];\n"
        b"\tunsigned\twidth[65];\n"
        b"\tbyte\t\tcode[];\n"
        b"}\tt_compscale;",
        b"typedef struct\n"
        b"{\n"
        b"\tuint16_t\tcodeofs[65];\n"
        b"\tuint16_t\twidth[65];\n"
        b"\tbyte\t\tcode[];\n"
        b"}\tt_compscale;",
    )
    data = data.replace(
        b"typedef struct\n"
        b"{\n"
        b"\tunsigned\tleftpix,rightpix;\n"
        b"\tunsigned\tdataofs[64];\n"
        b"// table data after dataofs[rightpix-leftpix+1]\n"
        b"}\tt_compshape;",
        b"typedef struct\n"
        b"{\n"
        b"\tuint16_t\tleftpix,rightpix;\n"
        b"\tuint16_t\tdataofs[64];\n"
        b"// table data after dataofs[rightpix-leftpix+1]\n"
        b"}\tt_compshape;",
    )
    data = re.sub(rb"extern[ \t]+unsigned[ \t]+far[ \t]*\*linecmds;",
                  b"extern\tuint16_t\tfar *linecmds;", data)
    data = re.sub(rb"unsigned[ \t]+far[ \t]*\*linecmds;",
                  b"uint16_t\tfar *linecmds;", data)
    data = re.sub(rb"unsigned[ \t]+far[ \t]*\*cmdptr;",
                  b"uint16_t\tfar *cmdptr;", data)
    data = data.replace(
        b"*(((unsigned *)&linescale)+1)=(unsigned)comptable;\t// seg of far call",
        b"wolf3d_set_line_scale(comptable);",
    )
    data = data.replace(
        b"*(((unsigned *)&linecmds)+1)=(unsigned)shape;\t\t// seg of shape",
        b"wolf3d_set_line_shape(shape);",
    )
    data = data.replace(
        b"*((unsigned far *)code)++ = startpix*SCREENBWIDE;",
        b"*(unsigned *)code = startpix*SCREENBWIDE;\n\t\t\tcode += sizeof(unsigned);",
    )
    data = data.replace(
        b"work->codeofs[src] = FP_OFF(code);",
        b"work->codeofs[src] = (unsigned)(code - (byte far *)work);",
    )
    data = data.replace(
        b"totalsize = FP_OFF(code);",
        b"totalsize = (unsigned)(code - (byte far *)work);",
    )
    data = data.replace(
        b"#ifndef JAPAN\n"
        b"\tif (!NoWait)\n"
        b"\t\tPG13 ();\n"
        b"#endif\n\n"
        b"#endif\n\n"
        b"\twhile (1)\n\t{",
        b"#ifndef JAPAN\n"
        b"\tif (!NoWait)\n"
        b"\t\tPG13 ();\n"
        b"#endif\n\n"
        b"#endif\n\n"
        b"#ifdef SMALLOS_WOLF3D_SOURCE_PROBE\n"
        b"\t{\n"
        b"\t\textern int wolf3d_probe_stop_after_demo_prelude;\n"
        b"\t\textern int wolf3d_probe_stop_after_title_frame;\n"
        b"\t\tif (wolf3d_probe_stop_after_demo_prelude)\n"
        b"\t\t\treturn;\n"
        b"\t\tif (wolf3d_probe_stop_after_title_frame)\n"
        b"\t\t\tNoWait = false;\n"
        b"\t}\n"
        b"#endif\n\n\twhile (1)\n\t{",
    )
    data = data.replace(
        b"\t\t\tVW_FadeIn();\n"
        b"#endif\n"
        b"\t\t\tif (IN_UserInput(TickBase*15))",
        b"\t\t\tVW_FadeIn();\n"
        b"#endif\n"
        b"#ifdef SMALLOS_WOLF3D_SOURCE_PROBE\n"
        b"\t\t\t{\n"
        b"\t\t\t\textern int wolf3d_probe_stop_after_title_frame;\n"
        b"\t\t\t\tif (wolf3d_probe_stop_after_title_frame)\n"
        b"\t\t\t\t\treturn;\n"
        b"\t\t\t}\n"
        b"#endif\n"
        b"\t\t\tif (IN_UserInput(TickBase*15))",
    )
    data = data.replace(
        b"(unsigned)linecmds = *cmdptr--;",
        b"linecmds = (uint16_t far *)((byte far *)shape + *cmdptr--);",
    )
    data = data.replace(
        b"(unsigned)linecmds = *cmdptr++;",
        b"linecmds = (uint16_t far *)((byte far *)shape + *cmdptr++);",
    )
    data = data.replace(b"(long)ssSample = 0;", b"ssSample = NULL;")
    data = data.replace(b"(long)pcSound = 0;", b"pcSound = NULL;")
    data = data.replace(b"(long)alSound = 0;", b"alSound = NULL;")
    data = data.replace(
        b'#include "GFXV_WL6.H"\n#include "AUDIOWL6.H"\n#include "MAPSWL6.H"',
        b'#ifdef UPLOAD\n#include "GFXV_WL1.H"\n#include "AUDIOWL6.H"\n#include "MAPSWL1.H"\n#else\n#include "GFXV_WL6.H"\n#include "AUDIOWL6.H"\n#include "MAPSWL6.H"\n#endif',
    )
    data = data.replace(b'#include "ID_HEAD.H"', b'#include "ID_HEADS.H"')
    data = data.replace(
        b"typedef struct\n"
        b"{\n"
        b"  unsigned bit0,bit1;\t// 0-255 is a character, > is a pointer to a node\n"
        b"} huffnode;",
        b"typedef struct\n"
        b"{\n"
        b"  uint16_t bit0,bit1;\t// 0-255 is a character, > is a node number\n"
        b"} huffnode;",
    )
    data = data.replace(
        b"typedef struct\n"
        b"{\n"
        b"\tunsigned\tRLEWtag;\n"
        b"\tlong\t\theaderoffsets[100];\n"
        b"\tbyte\t\ttileinfo[];\n"
        b"} mapfiletype;",
        b"typedef struct\n"
        b"{\n"
        b"\tuint16_t\tRLEWtag;\n"
        b"\tint32_t\t\theaderoffsets[100];\n"
        b"\tbyte\t\ttileinfo[];\n"
        b"} __attribute__((packed)) mapfiletype;",
    )
    data = data.replace(
        b"typedef\tstruct\n"
        b"{\n"
        b"\tlong\t\tplanestart[3];\n"
        b"\tunsigned\tplanelength[3];\n"
        b"\tunsigned\twidth,height;\n"
        b"\tchar\t\tname[16];\n"
        b"} maptype;",
        b"typedef\tstruct\n"
        b"{\n"
        b"\tint32_t\t\tplanestart[3];\n"
        b"\tuint16_t\tplanelength[3];\n"
        b"\tuint16_t\twidth,height;\n"
        b"\tchar\t\tname[16];\n"
        b"} __attribute__((packed)) maptype;",
    )
    data = data.replace(b"unsigned\t_seg\t*mapsegs[MAPPLANES];",
                        b"uint16_t\t_seg\t*mapsegs[MAPPLANES];")
    data = data.replace(b"extern\tunsigned\t_seg\t*mapsegs[MAPPLANES];",
                        b"extern\tuint16_t\t_seg\t*mapsegs[MAPPLANES];")
    data = data.replace(
        b"void\tCAL_CarmackExpand (unsigned far *source, unsigned far *dest,",
        b"void\tCAL_CarmackExpand (uint16_t far *source, uint16_t far *dest,",
    )
    data = data.replace(
        b"void CA_RLEWexpand (unsigned huge *source, unsigned huge *dest,long length,\n  unsigned rlewtag);",
        b"void CA_RLEWexpand (uint16_t huge *source, uint16_t huge *dest,long length,\n  uint16_t rlewtag);",
    )
    data = re.sub(rb"unsigned([ \t]+far[ \t]*\*)map", rb"uint16_t\1map", data)
    data = re.sub(rb"unsigned([ \t]+far[ \t]*\*)start", rb"uint16_t\1start", data)
    data = data.replace(b"*(unsigned far *)demoptr = length;",
                        b"*(uint16_t far *)demoptr = length;")
    data = data.replace(b"\tunsigned\tfar\t*source;\n#ifdef CARMACIZED",
                        b"\tuint16_t\tfar\t*source;\n#ifdef CARMACIZED")
    data = data.replace(
        b"CAL_CarmackExpand (source, (unsigned far *)buffer2seg,expanded);",
        b"CAL_CarmackExpand (source, (uint16_t far *)buffer2seg,expanded);",
    )
    data = data.replace(
        b"CA_RLEWexpand (((unsigned far *)buffer2seg)+1,*dest,size,",
        b"CA_RLEWexpand (((uint16_t far *)buffer2seg)+1,(uint16_t far *)*dest,size,",
    )
    data = data.replace(
        b"CA_RLEWexpand (source+1, *dest,size,",
        b"CA_RLEWexpand (source+1, (uint16_t far *)*dest,size,",
    )
    data = data.replace(
        b"typedef struct\n"
        b"{\n"
        b"\tint width,height;\n"
        b"} pictabletype;\n"
        b"\n"
        b"\n"
        b"typedef struct\n"
        b"{\n"
        b"\tint height;\n"
        b"\tint location[256];\n"
        b"\tchar width[256];\n"
        b"} fontstruct;",
        b"typedef struct\n"
        b"{\n"
        b"\tint16_t width,height;\n"
        b"} pictabletype;\n"
        b"\n"
        b"\n"
        b"typedef struct\n"
        b"{\n"
        b"\tint16_t height;\n"
        b"\tint16_t location[256];\n"
        b"\tchar width[256];\n"
        b"} fontstruct;",
    )
    if b"IGRAB-ed on Sun May 03 01:19:32 1992" in data and b"#define NUMCHUNKS    556" in data:
        data = build_wl1_data_graphics_header()
    if b"ORDERSCREEN=554" in data and b"#define NUMCHUNKS    556" in data:
        data = data.replace(
            b"\t\tORDERSCREEN=554,\n\t\tERRORSCREEN,                 // 555",
            b"\t\tORDERSCREEN=139,\n\t\tERRORSCREEN,                 // 140",
        )
        data = data.replace(b"#define NUMCHUNKS    556", b"#define NUMCHUNKS    156")
        data = data.replace(b"#define NUMTILE8     72", b"#define NUMTILE8     0")
        data = data.replace(b"#define NUMTILE16    144", b"#define NUMTILE16    0")
        data = data.replace(b"#define NUMTILE16M   270", b"#define NUMTILE16M   0")
        data = data.replace(b"#define NUMEXTERNS   2", b"#define NUMEXTERNS   17")
        data = data.replace(b"#define STARTTILE8M  140", b"#define STARTTILE8M  139")
        data = data.replace(b"#define STARTTILE16  140", b"#define STARTTILE16  139")
        data = data.replace(b"#define STARTTILE16M 284", b"#define STARTTILE16M 139")
        data = data.replace(b"#define STARTTILE32  554", b"#define STARTTILE32  139")
        data = data.replace(b"#define STARTTILE32M 554", b"#define STARTTILE32M 139")
        data = data.replace(b"#define STARTEXTERNS 554", b"#define STARTEXTERNS 139")
    if b"ORDERSCREEN=557" in data and b"#define NUMCHUNKS    558" in data:
        data = data.replace(
            b"\t\tORDERSCREEN=557,\n\t\tENUMEND",
            b"\t\tORDERSCREEN=147,\n\t\tENUMEND",
        )
        data = data.replace(b"#define NUMCHUNKS    558", b"#define NUMCHUNKS    156")
        data = data.replace(b"#define NUMTILE8     72", b"#define NUMTILE8     0")
        data = data.replace(b"#define NUMTILE16    144", b"#define NUMTILE16    0")
        data = data.replace(b"#define NUMTILE16M   270", b"#define NUMTILE16M   0")
        data = data.replace(b"#define NUMEXTERNS   1", b"#define NUMEXTERNS   9")
        data = data.replace(b"#define STARTTILE8   142", b"#define STARTTILE8   147")
        data = data.replace(b"#define STARTTILE8M  143", b"#define STARTTILE8M  147")
        data = data.replace(b"#define STARTTILE16  143", b"#define STARTTILE16  147")
        data = data.replace(b"#define STARTTILE16M 287", b"#define STARTTILE16M 147")
        data = data.replace(b"#define STARTTILE32  557", b"#define STARTTILE32  147")
        data = data.replace(b"#define STARTTILE32M 557", b"#define STARTTILE32M 147")
        data = data.replace(b"#define STARTEXTERNS 557", b"#define STARTEXTERNS 147")
    data = data.replace(
        b"\telse\n\t{\n\t CA_CacheGrChunk (ERRORSCREEN);\n\t screen = grsegs[ERRORSCREEN];\n\t}",
        b"\telse\n\t{\n\t if (grstarts)\n\t {\n\t  CA_CacheGrChunk (ERRORSCREEN);\n\t  screen = grsegs[ERRORSCREEN];\n\t }\n\t else\n\t  screen = nil;\n\t}",
    )
    data = data.replace(
        b"CP_iteminfo\n"
        b"\tMainItems={MENU_X,MENU_Y,10,STARTITEM,24},",
        b"CP_iteminfo\n"
        b"#if defined(GOODTIMES) || defined(SPEAR)\n"
        b"\tMainItems={MENU_X,MENU_Y,9,STARTITEM,24},\n"
        b"#else\n"
        b"\tMainItems={MENU_X,MENU_Y,10,STARTITEM,24},\n"
        b"#endif",
    )
    data = data.replace(
        b"#ifndef SPEAR\n"
        b"#define MENU_H\t13*10+6\n"
        b"#else\n"
        b"#define MENU_H\t13*9+6\n"
        b"#endif",
        b"#if !defined(SPEAR) && !defined(GOODTIMES)\n"
        b"#define MENU_H\t13*10+6\n"
        b"#else\n"
        b"#define MENU_H\t13*9+6\n"
        b"#endif",
    )
    return data


def copy_tree(src: pathlib.Path, dst: pathlib.Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    for path in src.iterdir():
        out = dst / path.name
        if path.is_dir():
            copy_tree(path, out)
            continue
        data = path.read_bytes()
        if path.suffix.upper() in {".C", ".H", ".EQU"}:
            data = normalize_c_source(data)
        out.write_bytes(data)
        lower = dst / path.name.lower()
        if lower != out and path.suffix.upper() in {".C", ".H", ".EQU"}:
            lower.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Prepare a generated Wolf3D source mirror for native builds"
    )
    parser.add_argument("src", type=pathlib.Path)
    parser.add_argument("dst", type=pathlib.Path)
    args = parser.parse_args()

    if args.dst.exists():
        shutil.rmtree(args.dst)
    copy_tree(args.src, args.dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
