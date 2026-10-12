"""Read ELF loader metadata and preserve Android SONAME version associations."""

import mmap
from pathlib import Path
import struct


class ElfMetadata:
    def __init__(self, data):
        self.data = data
        if data[:6] != b'\x7fELF\x02\x01':
            raise ValueError('Expected little-endian ELF64')
        self.loads = []
        dynamic = None
        offset = self.unpack('<Q', 32)[0]
        size, count = self.unpack('<HH', 54)
        if size != 56:
            raise ValueError('Unexpected ELF64 program header size')
        for index in range(count):
            kind, _, start, address, _, length, _, _ = self.unpack('<IIQQQQQQ', offset + size * index)
            if kind == 1:  # PT_LOAD
                self.loads.append((address, start, length))
            elif kind == 2:  # PT_DYNAMIC
                if dynamic is not None:
                    raise ValueError('Multiple ELF dynamic tables')
                dynamic = (start, length)
        if dynamic is None:
            raise ValueError('Missing ELF dynamic table')
        self.tags = {}
        for cursor in range(dynamic[0], dynamic[0] + dynamic[1], 16):
            tag, value = self.unpack('<qQ', cursor)
            if tag == 0:
                break
            self.tags.setdefault(tag, []).append(value)
        else:
            raise ValueError('Unterminated ELF dynamic table')
        self.string_size = self.tag(10)  # DT_STRSZ
        self.string_offset = self.file_offset(self.tag(5), self.string_size)  # DT_STRTAB

    def unpack(self, pattern, offset):
        if offset < 0 or offset + struct.calcsize(pattern) > len(self.data):
            raise ValueError('ELF record outside file')
        return struct.unpack_from(pattern, self.data, offset)

    def tag(self, name, default=None):
        values = self.tags.get(name, [])
        if not values and default is not None:
            return default
        if len(values) != 1:
            raise ValueError(f'Missing or repeated ELF dynamic tag: {name:#x}')
        return values[0]

    def file_offset(self, address, size=1):
        positions = {start + address - virtual for virtual, start, length in self.loads
                     if virtual <= address and address + size <= virtual + length}
        if len(positions) != 1:
            raise ValueError('ELF dynamic address has no unique file mapping')
        offset = positions.pop()
        if offset < 0 or offset + size > len(self.data):
            raise ValueError('ELF dynamic address outside file')
        return offset

    def string(self, offset):
        if not 0 <= offset < self.string_size:
            raise ValueError('ELF string outside DT_STRTAB')
        start = self.string_offset + offset
        end = self.data.find(b'\0', start, self.string_offset + self.string_size)
        if end < 0:
            raise ValueError('Unterminated ELF dynamic string')
        return self.data[start:end].decode('utf-8')

    def version_needs(self):
        count = self.tag(0x6fffffff, 0)  # DT_VERNEEDNUM
        if not count:
            return []
        cursor = self.file_offset(self.tag(0x6ffffffe), 16)  # DT_VERNEED
        records = []
        seen = set()
        for index in range(count):
            if cursor in seen:
                raise ValueError('Cyclic ELF version needs')
            seen.add(cursor)
            version, entries, name, auxiliary, next_record = self.unpack('<HHIII', cursor)
            if version != 1:
                raise ValueError('Unsupported ELF version need format')
            versions = []
            aux = cursor + auxiliary
            for item in range(entries):
                digest, flags, number, label, next_aux = self.unpack('<IHHII', aux)
                versions.append({'name': self.string(label), 'hash': digest,
                                 'flags': flags, 'index': number})
                if item + 1 < entries and not next_aux:
                    raise ValueError('Truncated ELF version need auxiliaries')
                aux += next_aux
            records.append({'file': self.string(name), 'versions': versions,
                            'name_field': cursor + 4})
            if index + 1 < count and not next_record:
                raise ValueError('Truncated ELF version needs')
            cursor += next_record
        return records

    def version_definitions(self):
        count = self.tag(0x6ffffffd, 0)  # DT_VERDEFNUM
        if not count:
            return []
        cursor = self.file_offset(self.tag(0x6ffffffc), 20)  # DT_VERDEF
        records = []
        seen = set()
        for index in range(count):
            if cursor in seen:
                raise ValueError('Cyclic ELF version definitions')
            seen.add(cursor)
            version, flags, number, entries, digest, auxiliary, next_record = self.unpack('<HHHHIII', cursor)
            if version != 1 or entries < 1:
                raise ValueError('Unsupported ELF version definition format')
            name, _ = self.unpack('<II', cursor + auxiliary)
            records.append({'name': self.string(name), 'hash': digest,
                            'flags': flags, 'index': number})
            if index + 1 < count and not next_record:
                raise ValueError('Truncated ELF version definitions')
            cursor += next_record
        return records

    def describe(self):
        soname = self.tag(14, 0)  # DT_SONAME
        runpath = self.tag(29, 0)  # DT_RUNPATH
        return {'soname': self.string(soname) if soname else '',
                'runpath': self.string(runpath) if runpath else '',
                'needed': [self.string(offset) for offset in self.tags.get(1, [])],
                'version_needs': [{key: value for key, value in record.items() if key != 'name_field'}
                                  for record in self.version_needs()],
                'version_definitions': self.version_definitions()}


def metadata(path):
    with Path(path).open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        return ElfMetadata(data).describe()


def verify_version_associations(path, providers, system_libraries):
    """Check direct-child SONAME lookup and private version definitions."""
    consumer = metadata(path)
    descriptions = {}
    for dependency, provider in providers.items():
        description = metadata(provider)
        if not description['soname'] or description['soname'] != dependency:
            raise ValueError(f'Android dependency SONAME mismatch: {dependency} in {path}')
        descriptions[description['soname']] = description
    for record in consumer['version_needs']:
        reference = record['file']
        if reference in descriptions:
            definitions = {(version['name'], version['hash'])
                           for version in descriptions[reference]['version_definitions']}
            for version in record['versions']:
                if (version['name'], version['hash']) not in definitions:
                    raise ValueError(f'Missing private symbol version {reference}:{version["name"]} in {path}')
        elif reference not in system_libraries or reference not in consumer['needed']:
            raise ValueError(f'Android verneed SONAME mismatch: {reference} in {path}')
    return consumer['version_needs']


def verify_runpath(path, providers):
    """Resolve each packaged SONAME through the consumer's actual RUNPATH."""
    path = Path(path).resolve()
    directories = []
    for component in metadata(path)['runpath'].split(':'):
        if component == '$ORIGIN':
            directories.append(path.parent)
        elif component.startswith('$ORIGIN/'):
            directories.append(path.parent / component[len('$ORIGIN/'):])
        else:
            raise ValueError(f'Unsupported private RUNPATH component {component} in {path}')
    for dependency, provider in providers.items():
        for directory in directories:
            candidate = directory / dependency
            if candidate.is_file():
                if candidate.resolve() != Path(provider).resolve():
                    raise ValueError(f'RUNPATH selects a different provider for {dependency} in {path}')
                break
        else:
            raise ValueError(f'RUNPATH cannot resolve private dependency {dependency} in {path}')
