#!/usr/bin/env python3

import argparse
import re
import sys

from typing import Dict, Union


class Capability:
    def __init__(self, name: str, value: Union[bool, int, str]):
        self._name = name
        self._value = value

    @property
    def name(self) -> str:
        return self._name

    @property
    def value(self) -> Union[bool, int, str]:
        return self._value

    def __lt__(self, other):
        return self._name < other._name

    def __le__(self, other):
        return self._name <= other._name

    def __eq__(self, other):
        return self._name == other._name

    def __ne__(self, other):
        return self._name != other._name

    def __gt__(self, other):
        return self._name > other._name

    def __ge__(self, other):
        return self._name >= other._name


class BoolCapability(Capability):
    def __init__(self, name: str):
        super().__init__(name, True)


class IntCapability(Capability):
    pass


class StringCapability(Capability):
    def __init__(self, name: str, value: str):
       # Expand \E to literal ESC in non-parameterized capabilities
        if '%' not in value:
            value = re.sub(r'\\E([0-7])', r'\\033" "\1', value)
            value = re.sub(r'\\E', r'\\033', value)
        else:
            # Need to double-escape \E in C string literals
            value = value.replace('\\E', '\\\\E')

        # Don’t escape ‘:’
        value = value.replace('\\:', ':')

        super().__init__(name, value)


class Fragment:
    def __init__(self, name: str, description: str):
        self._name = name
        self._description = description
        self._caps = {}

    @property
    def name(self) -> str:
        return self._name

    @property
    def description(self) -> str:
        return self._description

    @property
    def caps(self) -> Dict[str, Capability]:
        return self._caps

    def add_capability(self, cap: Capability):
        assert cap.name not in self._caps
        self._caps[cap.name] = cap

    def del_capability(self, name: str):
        del self._caps[name]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source_entry_name')
    parser.add_argument('source', type=argparse.FileType('r'))
    parser.add_argument('target_entry_name')
    parser.add_argument('target', type=argparse.FileType('w'))

    opts = parser.parse_args()
    source_entry_name = opts.source_entry_name
    target_entry_name = opts.target_entry_name
    source = opts.source
    target = opts.target

    lines = []
    for l in source.readlines():
        l = l.strip()
        if l.startswith('#'):
            continue
        lines.append(l)

    fragments = {}
    cur_fragment = None

    for m in re.finditer(
            r'(?P<name>(?P<entry_name>[-+\w@]+)\|(?P<entry_desc>.+?),)|'
            r'(?P<bool_cap>(?P<bool_name>\w+),)|'
            r'(?P<int_cap>(?P<int_name>\w+)#(?P<int_val>(0x)?[0-9a-fA-F]+),)|'
            r'(?P<str_cap>(?P<str_name>\w+)=(?P<str_val>(.+?)),)',
            ''.join(lines)):

        if m.group('name') is not None:
            name = m.group('entry_name')
            description = m.group('entry_desc')

            assert name not in fragments
            fragments[name] = Fragment(name, description)
            cur_fragment = fragments[name]

        elif m.group('bool_cap') is not None:
            name = m.group('bool_name')
            cur_fragment.add_capability(BoolCapability(name))

        elif m.group('int_cap') is not None:
            name = m.group('int_name')
            value = int(m.group('int_val'), 0)
            cur_fragment.add_capability(IntCapability(name, value))

        elif m.group('str_cap') is not None:
            name = m.group('str_name')
            value = m.group('str_val')
            cur_fragment.add_capability(StringCapability(name, value))

        else:
            assert False

    # Expand ‘use’ capabilities
    for frag in fragments.values():
        for cap in frag.caps.values():
            if cap.name == 'use':
                use_frag = fragments[cap.value]
                for use_cap in use_frag.caps.values():
                    frag.add_capability(use_cap)


            frag.del_capability(cap.name)
            break

    entry = fragments[source_entry_name]

    try:
        entry.del_capability('RGB')
    except KeyError:
        pass

    entry.add_capability(IntCapability('Co', 256))
    entry.add_capability(StringCapability('TN', target_entry_name))
    entry.add_capability(IntCapability('RGB', 8))  # 8 bits per channel

    terminfo_parts = []
    for cap in sorted(entry.caps.values()):
        name = cap.name
        value = str(cap.value)

        # Escape ‘“‘
        name = name.replace('"', '\"')
        value = value.replace('"', '\"')

        terminfo_parts.append(name)
        if isinstance(cap, BoolCapability):
            terminfo_parts.append('')
        else:
            terminfo_parts.append(value)

    terminfo = '\\0" "'.join(terminfo_parts)

    target.write('#pragma once\n')
    target.write('\n')
    target.write(f'static const char terminfo_capabilities[] = "{terminfo}";')
    target.write('\n')


if __name__ == '__main__':
    sys.exit(main())
