"""Read-only signatures/ABIs for Theresa UI/visibility helpers; no game launch."""
from pathlib import Path
import struct
import pefile
import capstone

root=Path(__file__).resolve().parents[3]
pe=pefile.PE(str(root/'test-install/Binaries/Win32/Fable Anniversary.exe'),fast_load=True)
data=pe.get_memory_mapped_image()
base=pe.OPTIONAL_HEADER.ImageBase
def seh(prefix,handler):
    return bytes.fromhex(prefix)+struct.pack('<I',base+handler)
signatures={
    0x01C2E130:seh('6a ff 68',0x02566628),
    0x01C2C3B0:seh('6a ff 68',0x025663E4),
    0x01C2EB60:seh('6a ff 68',0x02566728),
    0x01C2C5B0:seh('6a ff 68',0x02566448),
    0x00704E10:bytes.fromhex('8b 44 24 04 89 81 9c 02 00 00'),
    0x006878F0:seh('55 8b ec 83 e4 f0 6a ff 68',0x023CC5A0),
    0x001E1050:seh('6a ff 68',0x0239D003),
    0x0070C930:bytes.fromhex('55 8b ec 83 e4 f0'),
    0x001D1100:bytes.fromhex('8b 44 24 08 56 50 8b'),
    0x012B7800:bytes.fromhex('56 8b f1 c7 06 00 00 00 00'),
    0x012CC3C0:bytes.fromhex('8b 54 24 04 8b 02'),
    0x012B75D0:bytes.fromhex('56 57 8b f9 8b 37'),
    0x019F67C8:bytes.fromhex('6a 6c b8')+struct.pack('<I',base+0x02536685),
    0x019FC45B:bytes.fromhex('51 53 56 8b f1 57'),
    0x019F4DC9:bytes.fromhex('56 8d b1 b8 01 00 00'),
    0x01C49C90:bytes.fromhex('56 8b 74 24 08 85 f6'),
    0x019FF0EB:bytes.fromhex('e8')+struct.pack('<i',0x019F4F10-0x019FF0EB-5),
    0x019F4F10:bytes.fromhex('56 57 8b f1 e8')+struct.pack('<i',0x01C526B0-0x019F4F10-9),
}
for rva,expected in signatures.items():
    assert data[rva:rva+len(expected)]==expected,hex(rva)
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
for rva,size,cleanup in [(0x01C2E130,1096,'8'),(0x01C2C3B0,193,''),
                         (0x01C2EB60,952,'0xc'),(0x01C2C5B0,148,''),(0x00704E10,19,'4'),
                         (0x019F67C8,637,'4'),(0x019FC45B,241,''),(0x019F4DC9,25,''),(0x01C49C90,38,'4'),
                         (0x019F4F10,282,'4'),(0x006878F0,533,'4')]:
    returns=[ins.op_str for ins in md.disasm(data[rva:rva+size],base+rva) if ins.mnemonic=='ret']
    assert returns and all(value==cleanup for value in returns),(hex(rva),returns)
for rva,size,offset in [(0x01C2E130,1096,0x1CC),(0x01C2EB60,952,0x218)]:
    instructions=list(md.disasm(data[rva:rva+size],base+rva))
    assert any(ins.mnemonic=='mov' and ins.op_str.startswith('ecx, dword ptr [') and
               f'+ {hex(offset)}]' in ins.op_str for ins in instructions),(hex(rva),hex(offset))
print(f'PASS: {len(signatures)} retail helper signatures; SetMesh/destructor/parent-animation stack cleanup. No engine calls.')
