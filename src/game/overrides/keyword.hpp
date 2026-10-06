#pragma once
// Wizardmon's spell (WORD INPUT keyword, SAISEG): the US and JP keywords typed in half-width, and
// the port's completion codes CARDnnn / DIGIPARTnnn (keyword.cpp, docs/wiki/Playing.md).

#include <string>

namespace dcb {

/// What the last completion code gave, for the city line that announces it: `{gift}` (the item,
/// "Agumon Card" / "Digi-Part 12") and `{gift_more}` (a Digi-Part's effect; empty for a card).
/// city_text.cpp replaces both in English lines.
void keyword_expand_gift(std::string& line);

}  // namespace dcb
