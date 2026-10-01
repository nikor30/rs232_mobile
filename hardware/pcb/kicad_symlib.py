import sexpdata as sx
from sexpdata import Symbol as S

def load(path):
    with open(path) as f:
        return sx.loads(f.read())

def sym_name(node):
    if isinstance(node, list) and node and node[0]==S('symbol'):
        return node[1]
    return None

def top_symbols(lib):
    return [n for n in lib[1:] if isinstance(n, list) and n and n[0]==S('symbol')]

def find(lib, name):
    for n in top_symbols(lib):
        if sym_name(n) == name:
            return n
    return None

def pins_of_unit(unit_node):
    pins = []
    for item in unit_node[1:]:
        if isinstance(item, list) and item and item[0]==S('pin'):
            etype = str(item[1])
            at = next((x for x in item if isinstance(x,list) and x[0]==S('at')), None)
            length = next((x for x in item if isinstance(x,list) and x[0]==S('length')), None)
            name_ = next((x for x in item if isinstance(x,list) and x[0]==S('name')), None)
            num_ = next((x for x in item if isinstance(x,list) and x[0]==S('number')), None)
            pins.append({
                'etype': etype,
                'x': at[1], 'y': at[2], 'angle': at[3] if len(at)>3 else 0,
                'length': length[1] if length else None,
                'name': name_[1] if name_ else '',
                'number': num_[1] if num_ else '',
            })
    return pins
