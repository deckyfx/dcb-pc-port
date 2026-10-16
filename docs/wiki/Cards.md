# New cards

The port can add Digimon cards the game never had, numbered from 301 on. The first one is
**MetalTyrannomon** (301): Ultimate, Darkness, 1680 HP, support "If opponent's Specialty is Nature,
own Attack Power is tripled." How the game's code was mapped for this: [docs/re/cards.md](../re/cards.md).

## Getting the card

- **Wizardmon's spell** (Sky City, WORD INPUT): `CARD301`, while you have no copy of it
  ([Mods](Mods.md)).
- **Battle rewards**: like any card, by its rarity (`rarity` below) and the battle's reward class.
- **Trainer**: the General tab's presets **New cards x4** and **All cards x4** (turn on, save in game,
  turn off).

It shows in the Card Menu (the end of the list, the Darkness page, the totals and the "complete"
marks count it) and in Deck Edit, and plays in battle like any Digimon.

Not yet:
- **Fusion**: the Fusion Shop does not list it and no fusion gives it (its screens have tables of
  exactly 301 cards, to be mapped like the Card Menu's).
- **Auto deck** (Deck Edit) keeps to the original 301 cards.
- **Records**: a new card's best damage and KO counts are not kept (they read 0).
- **One new card** for now: the collection keeps one byte per card in the save, and only one spare
  byte follows the 301.

## Defining a card

Each card is an INI file in `config/SLPS-03101/cards/` (built into the program). The record starts as
a copy of an existing Digimon (`base`), then the keys below replace its fields:

```ini
[card]
number = 301               # 301, 302, ... with no gap
base = 8                   # the Digimon (0-190) copied first: MasterTyrannomon; its attack
                           # animations go with its battle model
name = MetalTyrannomon     # up to 20 letters
specialty = darkness       # fire | ice | nature | darkness | rare
level = ultimate           # rookie | armor | champion | ultimate
hp = 1680
dp = 50                    # DP to digivolve
plus_p = 10                # +P
fusion = 15                # fusion value
rarity = 30                # reward rarity (0: never a reward)
cross_effect = jamming     # none | first_attack | circle_to_0 | triangle_to_0 | cross_to_0 |
                           # counter_circle | counter_triangle | counter_cross | crash |
                           # eat_up_hp | jamming | x3_fire | x3_ice | x3_nature | x3_darkness | x3_rare
support_from = 120         # the card (0-190) whose support is copied: Devimon
model = 225                # battle model F:\225.PAK: an id the disc does not have (the free
                           # ids: docs/re/cards.md, "Art"); one it has is loaded instead
model_copy_of = 124        # served as a copy of F:\124.PAK until you give it its own
art = custom/cards/metaltyrannomon.png

[attack circle]
name = Giga Destroyer
power = 780
[attack triangle]
name = Nuclear Laser
power = 540
[attack cross]
name = Metal Fang
power = 300
```

## Art and model

None of this is in the repository (it is not ours to publish); it goes under `assets/SLPS-03101/`:

| File | What |
|---|---|
| `custom/cards/<art>` | the card's picture: any image stb_image reads (PNG, JPEG, BMP, ...; not WebP) |
| `files/F/<model>.PAK` | its own battle model; without it the port serves a copy of `model_copy_of` (its entries re-tagged with the new id, which the game looks them up by) |

From the image the port makes, as the files are read:
- the **card picture** (`B:\CARD\LCnnn.TIM`, 64x64, its own 256 colours): a plain background
  touching the image's edges (the white of wiki art) is cut out and the character is set, standing
  on the bottom edge, on a dark backdrop;
- the **mini card** (its entry in `B:\M_CARD.ARC`, 40x40, the battle's hand and board): in its
  group's shared palette, with the level badge the group's cards of that level have (the US ones
  with the English art);
- the **Deck Edit icon** (24x24, put in a free spot of VRAM): with the frame and level letter of the
  group's icons.

Without the image (a warning is logged) the card shows its base card's art.

## Debugging

`DCB_TRACE_CARDS=1` logs what the Card Menu / Deck Edit wrappers add; `DCB_VERIFY_REWARDS=1` runs the
game's own reward pick next to the port's on every reward and logs whether they agree.
