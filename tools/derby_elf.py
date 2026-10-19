"""Read a defined object and its owning source file from little-endian ELF."""
import struct


def object_symbol(elf, wanted, source_file=None):
    if len(elf)<64 or elf[:4]!=b'\x7fELF' or elf[5]!=1 or elf[4] not in (1,2):
        raise ValueError('expected little-endian ELF32/ELF64')
    wide=elf[4]==2
    shoff=struct.unpack_from('<Q' if wide else '<I',elf,40 if wide else 32)[0]
    step,count=struct.unpack_from('<HH',elf,58 if wide else 46)
    fmt='<IIQQQQIIQQ' if wide else '<IIIIIIIIII';symfmt='<IBBHQQ' if wide else '<IIIBBH'
    if step<struct.calcsize(fmt) or not count or shoff+step*count>len(elf):raise ValueError('invalid section table')
    sections=[struct.unpack_from(fmt,elf,shoff+i*step) for i in range(count)]
    def area(start,size):
        if start<0 or size<0 or start+size>len(elf):raise ValueError('section outside image')
        return elf[start:start+size]
    found=[]
    for section in sections:
        if section[1]!=2:continue
        link,entry=section[6],section[9]
        if link>=count or entry<struct.calcsize(symfmt) or section[5]%entry:raise ValueError('invalid symbol table')
        strings=sections[link];names=area(strings[4],strings[5]);symbols=area(section[4],section[5]);owner=None
        for pos in range(0,len(symbols),entry):
            row=struct.unpack_from(symfmt,symbols,pos)
            name,info,index,value,size=(row[0],row[1],row[3],row[4],row[5]) if wide else (row[0],row[3],row[5],row[1],row[2])
            end=names.find(b'\0',name)
            if name>=len(names) or end<0:raise ValueError('invalid symbol name')
            name=names[name:end].decode()
            if info&15==4:owner=name.rsplit('/',1)[-1]
            if name!=wanted or (source_file is not None and owner!=source_file):continue
            if info&15!=1 or index==0 or index>=count:raise ValueError('object missing a defined section')
            target=sections[index];delta=value-target[3]
            if delta<0 or delta+size>target[5]:raise ValueError('object outside section')
            data=None if target[1]==8 else area(target[4]+delta,size)
            found.append((size,data))
    if len(found)!=1:raise ValueError('object missing or ambiguous: '+wanted)
    return found[0]
