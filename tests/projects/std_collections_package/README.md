# Standard collections package gate

Build and run with the SDK compiler being tested, from this directory:

```powershell
vyxc --src=project . --target std_collections_package --run=aot -j20
```

The fixture checks primitive Vec values and an owning payload containing a
heap-backed String. It grows a Vec, inserts, replaces and removes elements,
then moves the Vec into a Box and mutates it through `deref_mut()`. A separate
Ref clone retains its payload until the last handle is destroyed.

The Dict path stores 80 heap-backed payloads, triggers rehashing, replaces an
entry, reads owned clones through get/try_get, creates a values snapshot,
removes and clears entries, and inserts a heap-backed default via get_ptr.
HashMap get/try_get/value snapshots and StringMap get exercise owned returns
through the public wrappers as well.

Stack/Queue paths grow buffers, wrap the queue head, transfer elements out,
clone reads and snapshots, replace entries, clear and reuse storage. The
three from_array APIs clone a two-element owning array and preserve its source.

Success prints `std.collections package ok` and exits zero. The owning paths
require 68 Vec/Ref payload destructions, 164 in Dict, six in HashMap/StringMap,
and 314 in Stack/Queue and raw-array imports.
Early destruction, missing cleanup or corrupted payload content returns a
nonzero code.
