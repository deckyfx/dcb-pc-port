# Name entry

The three text-entry screens of SLPS-03101 are one design copied into three overlays: the player
name (new game, `OPENSEG`), the deck name (Deck Select → Start, `SUBSEG`) and the WORD INPUT
keyword (a city event, `SAISEG`). The port opens all three on the letters page (ABC/123 at the
top of the tab list); `src/game/overrides/name_entry.cpp` does it.

## Layout

| | OPENSEG (player) | SUBSEG (deck) | SAISEG (keyword) |
|---|---|---|---|
| per-frame callback (`name_entry_frame`) | `801EB5BC` | `801E37B4` | `801EDC40` |
| pad input (`name_entry_input`) | `801EA910` | `801E2960` | `801ECF7C` |
| state (`g_name_entry`) | `801F9E18` | `801F6168` | `801F7BB0` |
| row table (`g_name_grid`) | `801F5118`, 27 rows | `801F541C`, 63 rows | `801F6C4C`, 27 rows |
| tab labels (string addresses) | `801E1BD8/BE4/BF0`, 決定 `BF8` | `801E1118/124/130/138/140/148/150`, 決定 `158`, 取り消し `160` | `801E1638/644/650`, 決定 `658`, 取り消し `660` |
| tab cursor width jump table | `801E1BAC` | `801E10EC` | `801E160C` |

- **Grid.** A window (content 270 px wide, 14 px per row) scrolled with `window_scroll_to`
  (`800171A8`). Each row is a pair of 5-character Shift-JIS strings from the row table
  (`{left, right}` pointers), sprintf'd into one line per row. Nine rows make a page: rows 0-8
  hiragana, 9-17 katakana, 18-26 full-width letters and digits; SUBSEG adds other symbols and
  three kanji pages (rows 27-62). SAISEG's table is followed by the secret keywords.
- **State.** `+0` character column 0-9, `+4` row, the name, its length, the focus (grid or tab
  list), the tab cursor (0-6 pages, 7 OK, 8 cancel) and the result (SAISEG has the same fields at
  other offsets, see the symbols). The entry task (SUBSEG `801E4450`, SAISEG `801EE704`) zeroes
  row and column, so the grid opens on row 0.
- **Tabs.** The labels are drawn at fixed y (+1, +15, +29 … +99 OK, +113 cancel) from
  code-immediate addresses; the highlighted one is `scroll y / page height`, i.e. the page in view.
  Confirming tab slot *n* < 7 sets row = 9n, column 0 and scrolls there. The tab cursor box comes
  from a 9-entry jump table: slots 0-1 are 48 px wide (four kana), 2-6 36 px, 7 24 px, 8 48 px.
  Up/Down move the tab cursor (wrapping), Left/Right go back into the grid; off either end of a
  grid row goes into the tab list. Pad bits 0x1/0x2 page the grid by 9 rows.

(Code reading of the three overlays, generated C with the MIPS comments; H for the addresses and
the table/label layout, M for the field meanings.)

## Override: letters first

`name_entry.cpp` overrides the three per-frame callbacks (`overrides.json`) and, around a call to
the original:

1. rotates rows 0-26 of the row table to letters, hiragana, katakana (in RAM, once per overlay
   load: row 0 tells whether it is done). Tab slot *n* still means page *n*, so the cursor opens
   on "Ａ", paging and the tab jumps follow the new order, and the highlight is right;
2. draws the first three labels in the same order through text aliases
   (`dcb::text_set_aliases`, honoured by the text draw override `8002AE00`): slot 0 draws the
   英数字 string, slot 1 ひらがな, slot 2 カタカナ. The strings cannot simply be swapped in RAM
   (slots of 12, 12 and 8 bytes; カタカナ needs 9). The catalog translates them by content as
   before. The aliases are set only while the callback runs;
3. gives slot 2 slot 0's cursor case (48 px) in the jump table, for the four-kana label.

The characters typed are the grid's own strings, so names and keywords are unchanged.

Verified headless (`DCB_SNAPSHOT`): new game → player name opens on the letters with the cursor
on A and "ABC" first and highlighted; typing A/B and OK → Yes keeps the name "AB". Save → Flame
City → Cards → Triangle (Edit Decks) → Start (Name): the deck name entry opens the same way; the
tab list jumps to Hiragana / Katakana pages with the right highlight and cursor width, typing and
OK rename the deck. The SAISEG keyword entry (a city event) was not reached in a headless run;
it runs the same code on its own addresses.
