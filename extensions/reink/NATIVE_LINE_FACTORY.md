# Native line creation without a selected seed

This contract is restricted to the ferrari / 3.28.0.169 / Qt 6.10.3 Xochitl
executable with SHA-256
`43a9d5d0acc5b998264c16586e11b848f3b83d2d63b5fd322b09c0977d94d3d4`.
It is based on read-only analysis of the exact executable, not the upstream
SceneAssistant SceneLineItem struct. No proprietary binary is distributed.

`RePaperNative::createNativeLineItem()` in `NativeLineFactory.{h,cpp}` calls the
complete native factory at ELF VA `0xe85a50` with its uint8 tag **5**. The return
type is an owned `std::shared_ptr<SceneItem>`; AAPCS64 passes its nontrivial
return storage in x8 and the tag in w0. There is no page, controller, worker,
selection or scene argument.

## Why this is a native allocation

- Entry `0xe85a58` masks the tag to 8 bits; `0xe85a64–0xe85a6c` selects tag 5.
- The complete branch `0xe85be4–0xe85c88` calls Xochitl's operator new for
  0xc0 bytes. It initializes its own shared control block and the 0xb0-byte
  SceneLineItem payload, including its native vtable, empty point list and
  default Line. It returns directly without accessing a scene or queue.
- Control-block vtable `0x10bb8c8` has RTTI `0x10bba90`, naming
  `std::_Sp_counted_ptr_inplace<SceneLineItem,...>`. The item vtable is
  `0x1682940`. Native shared ownership supplies allocation and destruction;
  the extension neither creates a vtable nor invents an item header.
- Both the binary scene reader (`0xe8ab28`) and JSON scene reader (`0xe91e10`)
  call this same factory before filling deserialized content.

The adapter checks the executable profile, GUI thread, runtime Line size 0x58
and item-list metatype size 24 before calling this address. Host builds always
return an error. The returned vtable must match the attested native type.

## Initialization after the factory

The native reader's empty factory initializes the two final words to zero.
The normal native drawing constructor instead initializes them to `(1, 0)`
at `0xe2b644–0xe2b654`. The first word at item +0xa0 is independently located by
the binary reader: it reads RM field 6 into a temporary at `0xeda400–0xeda404`,
loads that value at `0xeda4e0`, and stores it at `0xeda51c`. This is the line
timestamp; its native reader default is also 1 at `0xeda05c`.

The adapter initializes that timestamp to 1 on the private returned item, after
checking the expected zero factory default. It leaves item ID and parent ID at
+0x10/+0x18 zero. Native insertion assigns those identities. The final word at
+0xa8 remains the native zero default; no undocumented identity is generated.

The caller replaces only the attested Line subobject at +0x48, using ordinary
C++ assignment and Qt QList ownership, then supplies the complete list to the
native insertion command. The factory alone cannot publish a stroke or modify
an existing notebook. It works independently of any selected user's stroke.

## Geometry units and observed 0.2.2 issue

SolidPen tool 19 uses point width in quarter-units. Its modern renderer divides
the integer by 4, then by 2 to place either edge of the stroke. A desired
diameter `w` must therefore encode `round(w * 4)`; multiplying by 8 doubles it.
The proof chain is retained in the local `width-solidpen.md` and disassembly.

The upstream `Line.thickness` member name is misleading: native field +0x30
corresponds to the serialized starting length, as verified in the generated
0.2.2 records. New independent fragments should begin at 0. The point width
controls the diameter; the double at +0x28 is the thickness/mask scale.

Read-only inspection of the user's 0.2.2 page found the missing arrow shafts
stored as two-point paths of only a few coordinate units, while their heads
retained their fixed size. The native SolidPen renderer does accept two
distinct points: its state-1 branch emits a segment quad on the second point.
Increasing the point count would not repair a final gesture endpoint that
collapsed near its start. This finding points to input/coordinate handling,
separately from factory creation or memory ownership.

## Validation scope

The host factory gate test confirms that PC execution cannot reach the private
firmware address. The payload ownership tests independently check QList move
assignment, clone independence and QVariant/shared_ptr lifetime with fictitious
objects under ASan and UBSan. Neither host test pretends to execute Xochitl.
The real factory and insertion still require a controlled native runtime test;
no install or tablet write was performed while establishing this contract.

Local reproducible proof: `.local/qa/native-abi-3.28/native-line-factory-disassembly.txt`
and standalone build project `factory-src/` in the same directory.
