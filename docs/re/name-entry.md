# Name entry

The three text-entry screens of SLPS-03101 are one design copied into three overlays: the player
name (new game, `OPENSEG`), the deck name (Deck Select → Start, `SUBSEG`) and the WORD INPUT
keyword (a city event, `SAISEG`). The port opens all three on the letters page (ABC/123 at the
top of the tab list), and on the player and deck name screens that page types half-width letters,
so a name holds 12 of them; `src/game/overrides/name_entry.cpp` does both.

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
- **State.** `+0` character column 0-9, `+4` row, the name (`+9`, 13 bytes), its "length" (`+22`,
  really the cursor), the focus (grid or tab
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

The rotation alone leaves the typed characters alone (the grid's own full-width strings); the
next section changes that for names.

Verified headless (`DCB_SNAPSHOT`): new game → player name opens on the letters with the cursor
on A and "ABC" first and highlighted; typing A/B and OK → Yes keeps the name "AB". Save → Flame
City → Cards → Triangle (Edit Decks) → Start (Name): the deck name entry opens the same way; the
tab list jumps to Hiragana / Katakana pages with the right highlight and cursor width, typing and
OK rename the deck. The SAISEG keyword entry (a city event) was not reached in a headless run;
it runs the same code on its own addresses.

## Editing (JP) and half-width letters (port)

How the game edits the name (OPENSEG `801EB5BC`, SUBSEG `801E37B4`, after `name_entry_input`;
SAISEG the same at its own offsets). Everything is in 2-byte characters:

| Key | Focus | What the JP code does |
|---|---|---|
| circle | grid | writes the 2 bytes under the grid cursor at `name + 2*cursor`; with the first-edit flag (OPENSEG `+27`, SUBSEG `+28`, set by the entry task) and cursor 0, zeroes bytes 2-12 first (the first key replaces the name); cursor < 5 → cursor + 1, else focus = tab list on OK. Sound 1. |
| triangle | grid | shifts bytes `2*cursor .. 11` right by 2 (the last character falls off), then as circle (no first-edit clear). |
| cross | grid or tab list | cursor 0 → 1; bytes after `2*cursor` move left by 2 (the character before the cursor goes), cursor − 1. |
| L1 / R1 | (name box `801EBE18` / `801E417C`) | cursor − 1 / cursor + 1 while cursor ≠ 5 and a character is there. Sound 2. |
| — | name box | draws the name (`text_draw_grey`, clut 7, fixed cells) at window x + 1; underline box at x + 12·cursor, 12 wide (`cursor_set_target` `800193CC`, `cursor_draw` `80019448`, cursor objects OPENSEG `801F9D78`, SUBSEG `801F60C8`); SUBSEG adds the デック label at x + 76. |

Pads: OPENSEG reads the grid keys at pad `+10` (new presses), SUBSEG at `+14` (with auto-repeat)
of the pad state `*(8008C420 + 4·n)` (OPENSEG pad 0, SUBSEG pad `[+27]`); the tab list's circle is `+10` on both, L1/R1 `+14`. So a name is
at most 6 characters in 12 bytes + NUL: the player name at `game_data + 0` (copied back with
strcpy), a deck name at its record `+1` (13 bytes, DEK records `0x10C` apart from player `+0x2408`),
the keyword 6 characters.

**Port.** For the player and deck name the letters page types ASCII: Ａ-Ｚ → `A`-`Z`, ａ-ｚ →
`a`-`z`, ０-９ → `0`-`9`, its blank cells a space (the page has no punctuation). Kana, kanji and
the "other" page stay full-width, and names already saved are left as they are. A name holds 12
bytes as before, so up to 12 letters, and nothing downstream sees a longer string. Overrides:

- `name_entry_input` (OPENSEG `801EA910`, SUBSEG `801E2960`): the original runs first (grid
  cursor, paging, tab list), then, with the focus it leaves, circle / triangle / cross are done
  natively on bytes, and those buttons are cleared from both pad fields until the frame callback
  returns (its own edit code sees nothing; an edit that moves the focus to OK cannot also press OK).
  `+22` becomes a byte offset. Rules as the JP ones, by whole characters: circle replaces the
  character at the cursor (first-edit clear kept), triangle inserts (characters that no longer
  fit in 12 bytes fall off the end), cross deletes the one before the cursor; the cursor moves
  past a typed character while it stays below 12, otherwise (or when the character does not fit)
  the focus goes to OK.
- the name box callback (OPENSEG `801EBE18`, SUBSEG `801E417C`): the name is drawn one character
  at a time with `dcb::text_draw_verbatim` (ASCII with the US font, 6 px in fixed cells; SJIS
  through the JP original, 12 px; no catalog, so a name is never translated or taken for a
  message being typed out), L1/R1 step by whole characters (R1 up to byte 11), and the underline
  sits under the character at the cursor (6 or 12 wide; at the end, as wide as the page's
  characters). SUBSEG's デック label stays at x + 76: 12 letters end at x + 73.

The WORD INPUT keyword keeps full-width letters: the secret keywords it is compared with (after
the row table: ＪＩ２ＭＯＮ, ＭＴＬＥＴＥ, ＨーＫＡＢＵ …) are full-width and 6 characters long.

Where names are drawn (checked with an ASCII player name "SuperEagle12" and deck names
"FlameWarrior", "Aあ"; `DCB_TRACE_TEXT=hex` + snapshots): the name box, the reception's player-name panel, city lines with `*h0` (city_text.cpp: "Hi, I'm
SuperEagle12!", typewriter), the speaker line (`*c5` + name), the memory card (the new game
saves FILE 1 with the name; the load panel shows it and loading it reaches the city), the
battle cafe dialogue, the VS screen big name (bigname.cpp, 12 letters, 192 px), the battle field
name plates, Deck Select ("FlameWarrior Deck", "Aあ Deck": a letter next to kana now counts as
English in mixed strings, text.cpp `lone_letter`). All take the ASCII path (`out=en`). Not reached
headless: the Fusion Shop (`*h0`, event_text.cpp) and the tutorial (`*p`) - both paste the name
into an English line, the same path as the city lines; a renamed deck in battle (ASCII deck names
are the grafted US ones there already). A name inside a still-Japanese message is drawn by the
mixed-string path, where a JP code letter followed by a digit (`e1`, `c5`, `a3` …) is read as a
code: "Ace1" would lose its "e1" there. Translated messages carry the name in English and are not
affected.
