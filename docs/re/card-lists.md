# Card lists: paging and Left/Right

How the scrolling lists read the pad, which screens are card lists, and the port's Left/Right
paging (`src/game/overrides/list_paging.cpp`).

## Pad words

`pad_update` (`8001ABF4`) fills the pad state `*(8008C420 + 4·n)` once a frame: `+8` held, `+10`
pressed this frame, `+12` released, `+14` pressed with auto-repeat, copied from the live words
`+0..+6` that `8001A9E8` computes from the SIO reply. Bits (active high): L2 `0x1`, R2 `0x2`, L1
`0x4`, R1 `0x8`, triangle `0x10`, circle `0x20`, cross `0x40`, square `0x80`, select `0x100`,
start `0x800`, up `0x1000`, right `0x2000`, down `0x4000`, left `0x8000`. (H, disassembly)

## list_cursor_update (EXE `800199F4`)

The shared single-column list cursor, called once a frame by the window callback that owns the
list. List struct: `+0` window, `+4` cursor object, `+0x14` s16 cursor row, `+0x16` previous
row, `+0x20` s16 row count, `+0x25` u8 rows per page, `+0x26` u8 has the focus (0: cursor dimmed,
no input), `+0x27` u8 moved this frame, `+0x28` u8 pad index. With the focus and ≥ 2 rows it reads
only `+14` of its pad: up / down one row, **L2 / R2 one page** (the window scrolls with it). Left /
Right are never read. (H, disassembly; M for the field names)

Callers (return address → list): SUBSEG `801EAD84` Card Menu list and `801F2824` Deck Edit card
selection (both list `801F5CC4`), EVOSEG `801EA030` Fusion Shop card list (`801F2AC8`), SUBSEG
`801E8190` Edit Partner Digi-Parts list (`801F5C40`, found with `DCB_TRACE_LISTS=1`; `801E7F40` on
the same screen is the 3-row equipment list); the other
callers are short menus (sort orders, each followed by the card sort `8001C2EC`: SUBSEG `801EA6F8`
and `801EBA54`, EVOSEG `801E9B54`, OPENSEG `801E49FC`), other SUBSEG menus (`801E4A1C`,
`801E7F40`, `801E8190`), KAWSEG `801F0AB0` /
`801FA748`, and OPENSEG `801E6A10` (two card-row lists, one per pad, `801F4BDC + 0x2C·i`, in
"Battle with Friend"; not reached headless: it needs a second memory card). KAWSEG (`801F6D3C`,
`801F6FE0`, `801FA434`) and SAISEG (`801E4700`, `801F64A0`) carry their own copies of the
function.

## L1 / R1 in these screens

L1 / R1 do not page the card lists. They are read (pressed, `+10`) by:

- SUBSEG `801ED7DC` (window callback of the Card Menu's number panel): type page 0-7 (Fire, Ice, …;
  wraps), `801F7FD0 + 0x2C`; pad index `801F7E62`.
- SUBSEG `801F09F0` (Deck Edit input): the side panel between counts by type and by level. The same
  function moves the deck grid cursor with up / down / left / right (`+14`).
- the name box (`801E417C`, OPENSEG `801EBE18`, SAISEG `801EE488`): the name cursor (auto-repeat).

Verified headless (snapshots): in the Card Menu, R1 turns the panel Fire → Ice and the list does
not move; in Deck Edit's Card Selection R1 and Left / Right do nothing, in the Fusion Shop list
Left / Right do nothing; R2 pages both (000 → 008 / 009).

## Port: Left / Right page the card lists

`dcb_list_cursor_update` replaces `800199F4`. When the call returns to one of the three card-list
sites (return address, the `jal 800199F4` word before it and the delay-slot word that loads the
list's address, so another overlay at the same address does not match), it adds L2 to the list
pad's `+14` word when Left is in it, R2 for Right (neither when both), runs the original and puts
the word back. Only the list cursor sees the added bits: the Card Menu panel still turns with L1 /
R1 only, Deck Edit's grid and panel, name entry, menus and battle are untouched. This replaces the
earlier host-side mapping (Left/Right also pressed L1/R1 on every screen), which turned the Card
Menu panel, moved the name cursor in name entry and did not page any list.

Verified headless (`DCB_PAD_SCRIPT`, snapshots): Card Menu list, Right 000 → 008 → 016, Left back
to 008, panel stays Fire; Deck Edit grid, Right moves the cursor one slot, the panel stays on
counts by type; Card Selection, Right 000 → 008 → 016, Left back to 008; Fusion Shop, Right
000 → 009 → 018, Left back to 009; deck name entry, after typing "A" Right / Right / Left move the
grid cursor A → B → C → B while the name underline stays after the A.
