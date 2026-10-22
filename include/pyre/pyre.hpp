#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  pyre.hpp — within ε, everywhere; decode only what you need
// ═════════════════════════════════════════════════════════════════════════════
//
// An interpolative height pyramid with closed-loop, ε-limited prediction and
// adaptive binary arithmetic coding, in independent blocks of any size B.
//
// C++20 and the standard library only; no exceptions; integers only, so the
// same file gives the same heights on every machine. Copy include/pyre as it
// is. MIT licence.
//
//   tile.hpp       encode a tile; decode its head (to a level), a block, a window
//   container.hpp  the .pyre file: Builder (write), Reader (read, mapped or not)
//   params.hpp     the parameters and the status codes
//   walk.hpp, model.hpp, rc.hpp, crc32c.hpp: the parts

#include "container.hpp"
#include "crc32c.hpp"
#include "model.hpp"
#include "params.hpp"
#include "rc.hpp"
#include "tile.hpp"
#include "walk.hpp"
